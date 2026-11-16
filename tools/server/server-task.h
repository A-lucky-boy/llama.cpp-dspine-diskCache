#pragma once

#include <atomic>
#include "common.h"
#include "llama.h"
#include "server-disk-cache-codec.h"

#include <string>
#include <set>
#include <unordered_set>
#include <list>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <cstdint>
#include <functional>

// TODO: prevent including the whole server-common.h as we only use server_tokens
#include "server-common.h"
#include "server-disk-cache-match.h"

#include "server-disk-cache-quota.h"
#include "server-disk-cache-quarantine.h"
#include "server-prefix-index.h"
#include "server-prefix-materialize.h"
#include "mtmd-helper.h"


enum server_task_type {
    SERVER_TASK_TYPE_COMPLETION,
    SERVER_TASK_TYPE_EMBEDDING,
    SERVER_TASK_TYPE_RERANK,
    SERVER_TASK_TYPE_DECISION,
    SERVER_TASK_TYPE_INFILL,
    SERVER_TASK_TYPE_TRANSCRIBE_RT,
    SERVER_TASK_TYPE_REALTIME, // a conversation over /v1/realtime, see server-realtime.cpp
    SERVER_TASK_TYPE_CANCEL,
    SERVER_TASK_TYPE_CONTROL,
    SERVER_TASK_TYPE_NEXT_RESPONSE,
    SERVER_TASK_TYPE_METRICS,
    SERVER_TASK_TYPE_SLOT_GET,
    SERVER_TASK_TYPE_SLOT_SAVE,
    SERVER_TASK_TYPE_SLOT_RESTORE,
    SERVER_TASK_TYPE_SLOT_ERASE,
    SERVER_TASK_TYPE_GET_LORA,
    SERVER_TASK_TYPE_SET_LORA,
    SERVER_TASK_TYPE_TTS,
};

// TODO: change this to more generic "response_format" to replace the "format_response_*" in server-common
enum task_response_type {
    TASK_RESPONSE_TYPE_NONE, // llama.cpp native format
    TASK_RESPONSE_TYPE_OAI_CHAT,
    TASK_RESPONSE_TYPE_OAI_CMPL,
    TASK_RESPONSE_TYPE_OAI_RESP,
    TASK_RESPONSE_TYPE_OAI_ASR, // transcriptions API
    TASK_RESPONSE_TYPE_OAI_EMBD,
    TASK_RESPONSE_TYPE_ANTHROPIC,
};

enum stop_type {
    STOP_TYPE_NONE,
    STOP_TYPE_EOS,
    STOP_TYPE_WORD,
    STOP_TYPE_LIMIT,
};

enum server_control_action {
    SERVER_CONTROL_ACTION_NONE,
    SERVER_CONTROL_ACTION_REASONING_END,
    SERVER_CONTROL_ACTION_AUDIO_APPEND, // push control_audio into the slot's audio stream
    SERVER_CONTROL_ACTION_AUDIO_COMMIT, // flush the audio stream, more audio can follow
    SERVER_CONTROL_ACTION_AUDIO_END,    // flush the audio stream and end it
    // realtime conversations (SERVER_TASK_TYPE_REALTIME), applied in order with the audio actions
    SERVER_CONTROL_ACTION_TEXT_APPEND,     // append control_text to the conversation
    SERVER_CONTROL_ACTION_RESPONSE_CREATE, // generate after control_text, see control_response
    SERVER_CONTROL_ACTION_RESPONSE_CANCEL, // stop the response being generated
    SERVER_CONTROL_ACTION_CLEAR,           // drop the conversation
};

// a response of a realtime conversation
struct server_control_response {
    mtmd_helper_gen_audio_out_mode out_mode  = MTMD_HELPER_GEN_AUDIO_OUT_AUTO;
    common_params_sampling         sampling;       // of the text tokens: tool call grammar and preserved tokens
    int32_t                        n_predict = -1; // text tokens and audio frames, -1: until the context is full
};

struct task_params {
    bool stream          = false;
    bool include_usage   = false;
    bool cache_prompt    = true; // remember the prompt to avoid reprocessing all prompt
    bool return_tokens   = false;
    bool return_progress = false;

    int32_t sse_ping_interval = 30; // seconds between SSE comment pings while the stream stays silent, -1 disables

    int32_t n_keep    =  0; // number of tokens to keep from initial prompt
    int32_t n_discard =  0; // number of tokens after n_keep that may be discarded when shifting context, 0 defaults to half
    int32_t n_predict = -1; // new tokens to predict
    int32_t n_indent  =  0; // minimum line indentation for the generated text in number of whitespace characters
    int32_t n_cmpl    =  1; // number of completions to generate from this prompt

    int32_t n_cache_reuse = 0; // min chunk size to attempt reusing from the cache via KV shifting (0 = disabled)

    int64_t t_max_prompt_ms  = -1; // TODO: implement
    int64_t t_max_predict_ms = -1; // if positive, limit the generation phase to this time limit

    std::map<int, float> lora; // mapping adapter ID -> scale

    std::vector<std::string> antiprompt;
    std::vector<std::string> response_fields;

    bool timings_per_token   = false;
    bool post_sampling_probs = false;

    struct common_params_sampling sampling;
    struct common_params_speculative speculative;

    // response formatting
    bool               verbose  = false;
    task_response_type res_type = TASK_RESPONSE_TYPE_NONE;
    std::string        oaicompat_model;
    std::string        oaicompat_cmpl_id;

    // realtime control (SERVER_TASK_TYPE_CONTROL)
    server_control_action control_action = SERVER_CONTROL_ACTION_NONE;
    std::string           control_cmpl_id;
    std::vector<float>    control_audio; // PCM F32 samples for SERVER_CONTROL_ACTION_AUDIO_APPEND
    std::string           control_text;  // raw text, special tokens are parsed
    server_control_response control_response;

    // reported in generation_settings, parsing itself is owned by the chat session
    common_chat_format      chat_format      = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    common_reasoning_format reasoning_format = COMMON_REASONING_FORMAT_NONE;

    // message spans for checkpointing
    common_chat_msg_spans message_spans;

    // Embeddings
    int32_t embd_normalize = 2; // (-1=none, 0=max absolute int16, 1=taxicab, 2=Euclidean/L2, >2=p-norm)

    json format_logit_bias(const std::vector<llama_logit_bias> & logit_bias) const;
    json to_json(bool only_metrics = false) const;
};

// struct for tracking the state of a task (e.g., for streaming)
struct task_result_state {
    // tracking diffs for partial tool calls
    std::vector<common_chat_msg_diff> diffs;
    common_chat_session chat_session; // owns all parsing for this generation
    common_chat_msg chat_msg;
    std::vector<std::string> generated_tool_call_ids;
    std::unordered_set<size_t> sent_tool_call_names;

    // for OpenAI Responses and Anthropic streaming API:
    // track output item / content block state across chunks
    bool thinking_block_started = false;
    bool text_block_started = false;

    // for OpenAI Responses streaming API
    bool oai_resp_created = false;
    const std::string oai_resp_id;
    const std::string oai_resp_reasoning_id;
    const std::string oai_resp_message_id;
    std::string oai_resp_fc_id; // function call ID for current args delta

    task_result_state(common_chat_session session = {});

    // parse partial tool calls and update the internal state
    common_chat_msg update_chat_msg(
        const common_chat_input & added,
        bool is_partial,
        std::vector<common_chat_msg_diff> & diffs,
        bool filter_tool_calls = false);
};

struct server_task {
    int id = -1; // to be filled by server_queue

    // TODO @ngxson : remove this field and implement a mapping task_id -> idx in the response_reader
    size_t index = 0; // used when there are multiple prompts (batch request)

    // used by SERVER_TASK_TYPE_CANCEL
    int id_target = -1;
    int id_slot   = -1;

    // used by parallel sampling (multiple completions from same prompt)
    int id_parent  = -1;
    // temporary store of child tasks for scheduling
    // note: accessing to elements is invalid after the task is moved to server_slot
    std::vector<server_task> child_tasks;
    // if set on a parent, the children have their own prompt and only share its first n_tokens_shared tokens
    int32_t n_tokens_shared = 0;

    // used by SERVER_TASK_TYPE_INFERENCE
    task_params   params;
    server_tokens tokens;

    // only used by CLI, this allow tokenizing CLI inputs on server side
    // we need this because mtmd_context and vocab are not accessible outside of server_context
    bool                    cli = false;
    std::string             cli_prompt;
    std::vector<raw_buffer> cli_files;

    server_task_type type;

    // used by SERVER_TASK_TYPE_SLOT_SAVE, SERVER_TASK_TYPE_SLOT_RESTORE, SERVER_TASK_TYPE_SLOT_ERASE
    struct slot_action {
        int id_slot;
        std::string filename;
        std::string filepath;
    };
    slot_action slot_action;

    // used by SERVER_TASK_TYPE_METRICS
    bool metrics_reset_bucket = false;

    // used by SERVER_TASK_TYPE_DECISION
    // where to read the model output of each option, exactly one of the two lists is used
    struct decision {
        std::vector<llama_token> labels;       // logits of these tokens, at the last prompt token
        std::vector<int32_t>     label_groups; // if set, number of labels per output, the output is their max
        std::vector<int32_t>     markers;      // embeddings[column] at these prompt positions
        int32_t                  column = 0;
        // if set, embeddings is [q | k], and the output is instead the scaled dot product of q[pointer] and k[marker]
        int32_t                  pointer = -1;
        // if set, the prompt must be the only sequence of its batch
        bool                     no_batching = false;

        // if set, the outputs are the pooled embeddings of the sequence, one score per class
        bool                     pooled = false;

        // first prompt position that is read, -1 if none
        int32_t pos_first() const {
            int32_t pos = pointer;
            for (const int32_t marker : markers) {
                pos = pos < 0 ? marker : std::min(pos, marker);
            }
            for (size_t i = 0; i < order.size(); i++) {
                if (order[i] != 0) {
                    return pos < 0 ? (int32_t) i : std::min(pos, (int32_t) i);
                }
            }
            return pos;
        }

        // for a joint head: one value per prompt token, see llama_batch_ext_set_decision_order()
        // the scores are the first n_scores rows of the embeddings
        std::vector<int32_t> order;
        int32_t              n_scores = 0;
    };
    decision decision;

    // used by SERVER_TASK_TYPE_SET_LORA
    std::map<int, float> set_lora; // mapping adapter ID -> scale

    // used by SERVER_TASK_TYPE_TTS
    mtmd_helper::gen_audio::inp tts_inp;

    server_task() = default;

    server_task(server_task_type type) : type(type) {}

    int32_t n_tokens() const {
        return tokens.size();
    }

    bool need_embd() const {
        switch (type) {
            case SERVER_TASK_TYPE_EMBEDDING:
            case SERVER_TASK_TYPE_RERANK:
                return true;
            case SERVER_TASK_TYPE_DECISION:
                return !decision.markers.empty() || !decision.order.empty() || decision.pooled;
            default:
                return false;
        }
    }

    bool need_logits() const {
        switch (type) {
            case SERVER_TASK_TYPE_COMPLETION:
            case SERVER_TASK_TYPE_INFILL:
            case SERVER_TASK_TYPE_TRANSCRIBE_RT:
                return true;
            case SERVER_TASK_TYPE_DECISION:
                return !decision.labels.empty();
            default:
                return false;
        }
    }

    bool need_sampling() const {
        switch (type) {
            case SERVER_TASK_TYPE_COMPLETION:
            case SERVER_TASK_TYPE_INFILL:
            case SERVER_TASK_TYPE_TTS:
            case SERVER_TASK_TYPE_TRANSCRIBE_RT:
                return true;
            default:
                return false;
        }
    }

    // utility function
    static std::unordered_set<int> get_list_id(const std::vector<server_task> & tasks) {
        std::unordered_set<int> ids(tasks.size());
        for (size_t i = 0; i < tasks.size(); i++) {
            ids.insert(tasks[i].id);
            for (auto & child : tasks[i].child_tasks) {
                ids.insert(child.id);
            }
        }
        return ids;
    }

    void apply_chat_session(const common_chat_session & session) {
        if (!session.has_template()) {
            return;
        }

        session.apply_sampling(params.sampling);
        params.chat_format = session.format();
        params.antiprompt.insert(params.antiprompt.end(), session.additional_stops().begin(), session.additional_stops().end());
        params.message_spans = tokens.find_message_spans(session.message_delimiters());
    }

    void add_child(int id_parent, int id_child) {
        server_task copy;

        copy.id        = id_child;
        copy.id_parent = id_parent;
        copy.params    = params;
        copy.type      = type;
        copy.tokens    = tokens.clone();
        copy.id_slot   = -1; // child tasks cannot specify slot

        // use different sampling seed for each child
        // note: https://github.com/ggml-org/llama.cpp/pull/18700#discussion_r2675115723
        if (copy.params.sampling.seed != LLAMA_DEFAULT_SEED) {
            copy.params.sampling.seed += (uint32_t)child_tasks.size() + 1;
        }

        child_tasks.push_back(std::move(copy));
    }

    bool is_parent() const {
        return child_tasks.size() > 0;
    }

    bool is_child() const {
        return id_parent != -1;
    }

    // embedding and rerank tasks always process their whole prompt, their state is never reused
    bool is_stateless() const {
        return type == SERVER_TASK_TYPE_EMBEDDING || type == SERVER_TASK_TYPE_RERANK;
    }
};

struct result_prompt_progress {
    int32_t total = 0;
    int32_t cache = 0;
    int32_t processed = 0;
    int64_t time_ms = 0;

    json to_json() const;
};

struct server_task_result {
    int id           = -1;
    int id_slot      = -1;

    // TODO @ngxson : remove this field and implement a mapping task_id -> idx in the response_reader
    size_t index = 0; // to be used for batched tasks

    virtual bool is_error() {
        // only used by server_task_result_error
        return false;
    }
    virtual bool is_stop() {
        // only used by server_task_result_cmpl_*
        return true;
    }
    virtual void update(task_result_state &) {
        // only used by server_task_result_cmpl_*
    }
    virtual json to_json() = 0;
    virtual ~server_task_result() = default;
    virtual server_task_result * clone() const {
        GGML_ABORT("not implemented for this task type");
    }
};

// using shared_ptr for polymorphism of server_task_result
using server_task_result_ptr = std::unique_ptr<server_task_result>;

struct completion_token_output {
    llama_token tok;
    float prob;
    std::string text_to_send;
    struct prob_info {
        llama_token tok;
        std::string txt;
        float prob;
    };
    std::vector<prob_info> probs;

    json to_json(bool post_sampling_probs) const;

    static json probs_vector_to_json(const std::vector<completion_token_output> & probs, bool post_sampling_probs);

    static float logarithm(float x);

    static std::vector<unsigned char> str_to_bytes(const std::string & str);

};

struct server_task_result_cmpl_final : server_task_result {
    common_chat_input content;
    llama_tokens tokens;

    bool stream;
    bool include_usage;
    server_slot_stats stats;
    std::string prompt;

    bool truncated;
    int32_t n_decoded;
    int32_t n_prompt_tokens;
    int32_t n_prompt_tokens_cache;
    int32_t n_tokens_cached;
    bool has_new_line;
    std::string stopping_word;
    stop_type stop = STOP_TYPE_NONE;

    bool post_sampling_probs;
    std::vector<completion_token_output> probs_output;
    std::vector<std::string>  response_fields;

    task_params generation_params;

    // response formatting
    bool               verbose  = false;
    task_response_type res_type = TASK_RESPONSE_TYPE_NONE;
    std::string        oaicompat_model;
    std::string        oaicompat_cmpl_id;
    common_chat_msg    oaicompat_msg; // to be populated by update()

    std::vector<common_chat_msg_diff> oaicompat_msg_diffs; // to be populated by update()
    bool is_updated = false;

    // for OpenAI Responses API
    std::string oai_resp_id;
    std::string oai_resp_reasoning_id;
    std::string oai_resp_message_id;

    virtual bool is_stop() override {
        return true; // in stream mode, final responses are considered stop
    }

    virtual json to_json() override;

    virtual void update(task_result_state & state) override {
        is_updated = true;
        oaicompat_msg = state.update_chat_msg(content, false, oaicompat_msg_diffs);

        oai_resp_id = state.oai_resp_id;
        oai_resp_reasoning_id = state.oai_resp_reasoning_id;
        oai_resp_message_id = state.oai_resp_message_id;
    }

    json to_json_non_oaicompat();

    json usage_json_oaicompat();

    json to_json_oaicompat();

    json to_json_oaicompat_chat();

    json to_json_oaicompat_chat_stream();

    json to_json_oaicompat_resp();

    json to_json_oaicompat_resp_stream();

    json to_json_oaicompat_asr();

    json to_json_anthropic();

    json to_json_anthropic_stream();
};

struct server_task_result_cmpl_partial : server_task_result {
    common_chat_input content;
    llama_tokens      tokens;

    int32_t n_decoded;
    int32_t n_prompt_tokens;
    int32_t n_prompt_tokens_cache;

    bool post_sampling_probs;
    bool is_progress = false;
    bool is_begin = false; // whether to send 200 status to HTTP client (begin of SSE stream)
                           // ref: https://github.com/ggml-org/llama.cpp/pull/23884
    bool audio_commit_drained = false; // realtime: non-final commit has fully decoded
    completion_token_output prob_output;
    server_slot_stats stats;
    result_prompt_progress progress;

    // response formatting
    bool               verbose  = false;
    task_response_type res_type = TASK_RESPONSE_TYPE_NONE;
    std::string        oaicompat_model;
    std::string        oaicompat_cmpl_id;
    std::vector<common_chat_msg_diff> oaicompat_msg_diffs; // to be populated by update()
    bool is_updated = false;

    // Streaming state copied from task_result_state for this chunk
    bool thinking_block_started = false;
    bool text_block_started     = false;

    // for OpenAI Responses API
    bool oai_resp_created = false;
    std::string oai_resp_id;
    std::string oai_resp_reasoning_id;
    std::string oai_resp_message_id;
    std::string oai_resp_fc_id;

    // for Anthropic API: track if any reasoning content has been generated
    bool anthropic_has_reasoning = false;

    virtual bool is_stop() override {
        return false; // in stream mode, partial responses are not considered stop
    }

    virtual void update(task_result_state & state) override;

    virtual json to_json() override;

    json to_json_non_oaicompat();

    json to_json_oaicompat();

    json to_json_oaicompat_chat();

    json to_json_oaicompat_resp();

    json to_json_oaicompat_asr();

    json to_json_anthropic();
};

struct server_task_result_embd : server_task_result {
    std::vector<std::vector<float>> embedding;

    int32_t n_tokens;

    // response formatting
    task_response_type res_type = TASK_RESPONSE_TYPE_NONE;

    virtual json to_json() override;

    json to_json_non_oaicompat();

    json to_json_oaicompat();
};

// one event of a realtime conversation: text and audio of the response being generated, or its end
struct server_task_result_realtime : server_task_result {
    std::string        text;
    std::vector<float> audio;               // PCM F32 at the sample rate of mtmd_gen_audio_get_info()
    bool               response_done = false;
    bool               cancelled     = false;
    int32_t            n_decoded     = 0;

    virtual bool is_stop() override {
        return false; // the session ends when the client leaves, the task is cancelled then
    }

    virtual json to_json() override;
};

struct server_task_result_tts : server_task_result {
    std::string audio; // raw bytes for this chunk (WAV or PCM, per request's out_type)
    int32_t     sample_rate = 0;
    bool        final = false; // true for the last chunk of a request

    virtual bool is_stop() override { return final; }

    virtual json to_json() override;
};

struct server_task_result_rerank : server_task_result {
    float score = -1e6;

    int32_t n_tokens;

    virtual json to_json() override;
};

struct server_task_result_decision : server_task_result {
    std::vector<float> scores; // one raw model output per option

    int32_t n_tokens;

    virtual json to_json() override;
};

struct server_task_result_error : server_task_result {
    error_type err_type = ERROR_TYPE_SERVER;
    std::string err_msg;

    // for ERROR_TYPE_EXCEED_CONTEXT_SIZE
    int32_t n_prompt_tokens = 0;
    int32_t n_ctx           = 0;

    virtual bool is_error() override {
        return true;
    }

    virtual json to_json() override;
};

// what actually happened to an entry file. only `removed` and `already_missing`
// mean the bytes are gone, so only those may reclaim quota
enum class disk_cache_remove_status {
    removed,
    already_missing,
    failed,   // sharing violation, permissions, ... - the file is still there
};

const char * disk_cache_remove_status_name(disk_cache_remove_status status);

// ownership of the file name a write is about to use. the name is derived from
// the content hash, so a hash hit is a *path* hit: it has to be resolved before
// anything is written, never overwritten blindly
// paths this capacity transaction must not lose. a set of copies, not of
// pointers: `entries` is a std::vector, so an erase() invalidates the storage of
// every element after the removed one
using disk_cache_protected_paths = std::unordered_set<std::string>;

enum class disk_cache_path_owner {
    free,       // nobody holds it, or a quarantined holder was reclaimed
    identical,  // a usable entry with the very same identity already holds it
    conflict,   // another prompt, or a quarantined holder that could not be freed
};

const char * disk_cache_path_owner_name(disk_cache_path_owner owner);

// why a lookup did not reuse any tokens. the names go into the --cache-stats
// log line and the /metrics label set, so only append new values
enum class cache_reject_reason : uint8_t {
    none = 0,
    no_prefix,      // no entry shared a prefix long enough to be restored
    too_short,      // the prompt or every candidate is below --cache-disk-min-tokens
    media_mismatch, // the media chunks at the shared positions differ
    io_cost,        // reading the entry would cost more than the prefill it saves
    checksum_error, // the entry file failed a checksum
    read_error,     // the entry file could not be read (transient)
    restore_failed, // the state could not be written into the contexts
    signature_mismatch, // the entry has a state this process cannot restore (e.g. a draft state without a draft model)
    count,
};

const char * cache_reject_reason_name(cache_reject_reason reason);

// outcome of one cache lookup; filled even when nothing was restored, so the
// caller can tell "no lookup", "no candidate", "candidate rejected" and
// "restore failed" apart
struct prompt_cache_restore_info {
    enum class source : uint8_t {
        none,
        ram,
        disk,
    };

    source  src  = source::none;
    size_t  entry_tokens  = 0; // tokens the served entry holds
    size_t  candidate_lcp = 0; // best lcp over all candidates, before the gates
    size_t  lcp           = 0; // lcp of the entry that was actually restored
    size_t  read_bytes    = 0;
    int64_t elapsed_us    = 0;

    cache_reject_reason reject = cache_reject_reason::none;
};

// cumulative counters of the on-disk tier, sampled by /metrics
struct disk_cache_counters {
    uint64_t n_lookups              = 0;
    uint64_t n_candidate_lcp_tokens = 0; // sum of the best candidate lcp, accepted or not
    uint64_t n_read_bytes           = 0;
    uint64_t n_restore_us           = 0;
    uint64_t n_write_bytes          = 0;
    uint64_t n_write_rejected       = 0; // entries rolled back because they did not fit, cumulative
    uint64_t n_rollback_failed      = 0; // rollbacks whose file could not be deleted and had to be quarantined
    uint64_t n_write_failed         = 0; // writes that did not commit, for any reason
    uint64_t n_write_orphaned       = 0; // failed writes that left a file at the final path, quarantined until reclaimed
    uint64_t n_quota_unreclaimed    = 0; // transactions that ended over quota because a physical removal failed

    // prefix materialization: scheduling and export are reported separately from a
    // real cache hit - only a committed disk entry makes the prefix reusable
    uint64_t n_materialize_scheduled = 0;
    uint64_t n_materialize_exported  = 0;
    uint64_t n_materialize_skipped   = 0;

    uint64_t n_manifest_fallback    = 0;
    uint64_t n_reject[(size_t) cache_reject_reason::count] = {};
    uint64_t n_enqueue_queued       = 0;
    uint64_t n_enqueue_dedup        = 0; // already stored, already queued or covered by a longer entry
    uint64_t n_enqueue_rejected     = 0; // over the size limit or the queue budget
    uint64_t n_queue_cancelled      = 0; // queued writes dropped because the disk tier stopped
    uint64_t n_remove_failed        = 0; // files that could not be deleted, cumulative
    uint64_t n_hash_collisions      = 0; // stored path with a different identity, cumulative
    uint64_t n_path_conflicts       = 0; // writes refused because the path stayed occupied, cumulative
    uint64_t n_quarantined          = 0; // entries currently excluded from lookups
    uint64_t n_size_unknown_paths_snapshot = 0; // live paths occupying space that could not be measured
    uint64_t n_entries              = 0;
    uint64_t n_bytes                = 0; // accounted disk usage (total_size)
    uint64_t n_queue_entries        = 0;
    uint64_t n_queue_bytes          = 0;
};

// used by /metrics API
struct server_task_result_metrics : server_task_result {
    // these are immediate stats, not accumulated (server_metrics is cumulative)
    int n_processing_slots = 0;
    int n_tasks_deferred = 0;

    server_metrics metrics;

    // counters of the on-disk prompt cache tier (zero when it is disabled)
    disk_cache_counters cache_disk;

    // counters of the optional prefix directory (zero when it is disabled)
    prefix_index_stats cache_prefix;

    // prefix materialization outcomes. these live in the context, not the disk
    // tier, because scheduling and export happen before anything reaches a file
    uint64_t n_materialize_scheduled = 0;
    uint64_t n_materialize_exported  = 0;
    uint64_t n_materialize_skipped   = 0;

    // pre-export admission: the metadata check settled it before the state was
    // exported, and the check let the export proceed. a saved export is not a cache
    // hit - it is never counted as a reused token
    uint64_t n_preflight_skipped     = 0;
    uint64_t n_preflight_allowed     = 0;

    virtual json to_json() override;

    struct metric_item {
        std::string name;
        std::string description;
        double value; // prometheus values are always float64
    };
    std::string to_metrics();
};

// used by /slots API
struct server_task_result_slots : server_task_result {
    int n_idle_slots = 0;

    // while we can also use std::vector<server_slot> this requires copying the slot object which can be quite messy
    // therefore, we use json to temporarily store the slot.to_json() result
    json slots_data = json::array();

    virtual json to_json() override;
};

struct server_task_result_slot_save_load : server_task_result {
    std::string filename;
    bool is_save; // true = save, false = load

    size_t n_tokens;
    size_t n_bytes;
    double t_ms;

    virtual json to_json() override;
};

struct server_task_result_slot_erase : server_task_result {
    size_t n_erased;

    virtual json to_json() override;
};

struct server_task_result_control : server_task_result {
    bool        success = false;
    std::string message; // optional detail when success is false

    virtual json to_json() override {
        json out = json { { "success", success } };
        if (!message.empty()) {
            out["message"] = message;
        }
        return out;
    }
};

struct server_task_result_get_lora : server_task_result {
    struct lora {
        common_adapter_lora_info info;
        std::string  alora_invocation_string;
        llama_tokens alora_invocation_tokens;
    };
    std::vector<lora> loras;

    virtual json to_json() override;
};

struct server_task_result_apply_lora : server_task_result {
    virtual json to_json() override;
};

struct server_prompt {
    server_tokens tokens;

    // a checkpoint does not change after it is created, so the prompt cache shares it with the slot instead of a copy
    std::list<std::shared_ptr<const common_prompt_checkpoint>> checkpoints;

    void clear() {
        tokens.clear();
        checkpoints.clear();
    }

    int n_tokens() const {
        return tokens.size();
    }

    server_prompt clone() const {
        return server_prompt {
            tokens.clone(),
            checkpoints,
        };
    }
};

struct server_prompt_data {
    // shared so the disk tier can mirror an entry without copying gigabytes of
    // KV state: the RAM cache and the pending write hold references to the
    // same buffers, and the memory is freed when both are gone
    std::shared_ptr<common_state_data> main;
    std::shared_ptr<common_state_data> drft;

    size_t size() const {
        return (main ? main->size() : 0) + (drft ? drft->size() : 0);
    }
};

struct server_prompt_cache_state {
    server_prompt prompt;
    server_prompt_data data;

    size_t size() const {
        size_t res = data.size();

        for (const auto & ckpt : prompt.checkpoints) {
            res += ckpt->size();
        }

        return res;
    }
};

struct server_disk_cache_entry {
    std::string  path;
    uint64_t     hash      = 0;
    llama_tokens tokens;      // plain token ids, media placeholders included
    std::vector<uint32_t> media_indices;
    std::vector<uint32_t> media_token_counts;
    std::vector<std::string> media_ids;
    size_t       size_main = 0;
    size_t       size_drft = 0;
    size_t       size_file = 0; // whole file, including the header
    int64_t      last_used = 0; // unix seconds
    bool         last_used_dirty = false; // in-memory last_used differs from the on-disk header
    // CRC64 of the packed token/media metadata, as recorded by the writer. the
    // lookup index only holds metadata, so without it a manifest record cannot
    // be bound to the entry file it claims to describe: read_entry compares this
    // value with the one in the file header and re-computes the CRC over the
    // bytes it actually read
    uint64_t     packed_crc = 0;

    // a file that could not be deleted (sharing violation, permissions) stays on
    // disk and keeps occupying its bytes, so its quota is not reclaimed; it is
    // excluded from lookups instead of being reported as freed
    bool         quarantined = false;

    // the path is known to occupy space but its size could not be measured, so
    // `size_file` is not a real figure. Kept separate from a measured zero: a zero
    // reads as "costs nothing", which would let the quota check admit writes into
    // space that is actually taken
    bool         size_unknown = false;
};

// result of reading one entry file. the caller must not write any KV state into
// the contexts unless the status is ok: only then have the metadata, the
// payload checksums and the packed<->KV binding been validated
enum class disk_cache_read_status {
    ok,
    corrupt,   // deterministic: the entry can never be restored, remove it
    io_error,  // transient: keep the entry, a later request may succeed
};

// one entry file decoded and validated, in the order the file stores it
struct server_disk_cache_payload {
    std::vector<char> packed;

    // how many physical bytes this read actually consumed: the stored segment
    // lengths plus the header and segment table, NOT the decoded sizes. An encoded
    // segment occupies less on disk than it decodes to, and a checkpoint that was
    // skipped was never read at all, so the entry's total file size is not the
    // answer either. read_entry() sets it; load() reports it.
    uint64_t stored_bytes = 0;

    llama_tokens tokens;
    std::vector<uint32_t> media_indices;
    std::vector<uint32_t> media_token_counts;
    std::vector<std::string> media_ids;
    common_state_data main;
    common_state_data drft;
    std::list<std::shared_ptr<const common_prompt_checkpoint>> checkpoints;
};

// how a write finished, in the terms the ledger needs.
//
// A plain bool is not enough. The file is renamed into place before its size is
// measured, so a write can fail *after* the final path exists. Reporting only "false"
// would leave the worker believing no file was produced, while a fully written
// snapshot sits on disk outside `entries` and `total_size` - and the next write of
// the same hash would find the path taken by something nothing is tracking.
//
// The distinction the caller needs is whether the final path is gone, still there, or
// of unknown state; only the first is an ordinary failure.
enum class disk_cache_write_outcome {
    committed,          // the file is on disk, measured, and may be indexed
    failed_clean,       // nothing was written, or the partial file was *confirmed* removed
    failed_orphaned,    // the final path exists, or its state could not be confirmed
};


// Where the data sitting at the final path came from.
//
// A failed rename leaves whatever was already there. The writer's byte count
// describes the *temporary* file it wrote, so charging that number to the final path
// would be sizing a file the writer never produced. Only when this attempt's own
// rename landed does that count say anything about what is on disk, and even then
// only as a floor.
enum class disk_cache_write_origin {
    none,                       // nothing of this attempt reached the final path
    this_attempt,               // this attempt's rename landed; written_bytes describes it
    preexisting_unverified,     // the final path held something older, not this write
};

struct disk_cache_write_result {
    disk_cache_write_outcome outcome = disk_cache_write_outcome::failed_clean;
    disk_cache_path_state    path_state = disk_cache_path_state::confirmed_absent;
    disk_cache_write_origin  origin = disk_cache_write_origin::none;

    // the final path, always set: the worker must be able to quarantine exactly this
    // path without recomputing it
    std::string path;

    // the real size when it could be measured. Zero when it could not - never a
    // guess: an estimated size must not be used to sign a recoverable snapshot into
    // the ledger as if it were measured.
    size_t size_file = 0;

    // the length the writer knows it wrote, from the stream position after flush.
    // It describes this attempt's own bytes only; `origin` says whether those bytes
    // are what sits at the final path
    size_t written_bytes = 0;

    // True when the path is charged to the ledger without a measured size. The
    // byte count for such a path is unknown, and treating unknown as zero would let
    // the quota check admit writes that do not fit.
    bool size_unknown = false;
};

// an entry queued for the background writer
struct server_disk_cache_pending {
    uint64_t           hash   = 0;
    std::vector<char>  packed;      // plain tokens plus media chunk indices
    llama_tokens       ids;         // plain token ids, with media placeholders
    std::vector<uint32_t> media_indices;
    std::vector<uint32_t> media_token_counts;
    std::vector<std::string> media_ids;
    std::shared_ptr<const common_state_data> main;
    std::shared_ptr<const common_state_data> drft;
    std::list<std::shared_ptr<const common_prompt_checkpoint>> checkpoints;
    size_t             bytes = 0;
};

// one record of the on-disk index file (see load_manifest/save_manifest)
struct disk_cache_manifest_meta {
    uint64_t            hash      = 0;
    uint64_t            size_file = 0;
    uint64_t            size_main = 0; // KV state payload sizes, cross-checked by read_entry
    uint64_t            size_drft = 0;
    int64_t             last_used = 0;
    uint64_t            packed_crc = 0; // writer-side checksum of the metadata below, bound to the entry file
    std::vector<char>   packed;   // prompt metadata, same encoding as in the entry header
};

// second-level (on-disk) tier of the prompt cache: entries evicted from the RAM
// cache are written in the background and restored when a new prompt shares a
// long enough prefix with them. Survives restarts.
struct server_disk_cache {
    server_disk_cache(const std::string & dir, const std::string & sig, int32_t limit_size_mib, int32_t min_tokens, int32_t ttl_seconds, int32_t queue_max_mib, disk_cache_codec_mode codec_mode = disk_cache_codec_mode::none);
    ~server_disk_cache();

    // restore the best matching entry (if any) into the contexts
    // returns false only when a restore was attempted and failed
    // info receives the lookup outcome (source, candidate lcp, reject reason)
    bool load(server_prompt & prompt, const server_tokens & tokens_new, llama_context * ctx_tgt, llama_context * ctx_dft, int32_t id_slot,
            prompt_cache_restore_info & info);

    // hand an evicted RAM entry over to the background writer
    void store(server_prompt_cache_state && state);

    // copy an entry that stays in the RAM cache to disk (idle flush)
    // returns true when a write was queued
    bool mirror(const server_prompt_cache_state & state, size_t budget_left);

    size_t n_entries() const;
    size_t size() const;

    // cumulative counters for /metrics; takes the lock to sample entries/queue
    disk_cache_counters stats() const;

    // copy of the committed entries, so the prefix directory can register the
    // snapshots that already exist on disk without touching the cache internals
    std::vector<server_disk_cache_entry> entries_snapshot() const;

    // optional hooks for the prefix directory. both are delivered **while the
    // cache lock is held**, in the same critical section that changed `entries`
    // and `total_size`, which makes the commit/invalidate order the physical
    // order - there is no second, asynchronous delivery path. the prefix index
    // never calls back into the cache, so there is no lock inversion.
    // the second argument is a monotone event epoch: a commit older than a
    // withdrawal of the same path is rejected by the index
    std::function<void(const server_disk_cache_entry &, uint64_t)> on_entry_committed;
    std::function<void(const std::string &, uint64_t)>            on_entry_invalidated;

    // counters written from the lookup path and the writer thread; the sizes of
    // `entries` and `queue` are sampled separately under the lock
    disk_cache_counters counters;

    std::string dir;
    std::string sig;

    // Whether the disk tier may still be used.
    //
    // Atomic rather than a plain bool, and this is not a formality. The tier is
    // disabled from the background writer, under the cache mutex, while the request
    // thread reads the flag on entry to load(), store(), mirror(),
    // preflight_materialize() and the flush/mirror helpers - none of which hold that
    // mutex. A plain bool read by one thread while another writes it is a data race,
    // whatever the intention behind it.
    //
    // The transitions are one-way: a cache that has concluded it cannot trust its own
    // metadata does not become trustworthy again in the same run. Release on the store
    // and acquire on the load is therefore the whole protocol - it is a shutdown
    // flag, not a shared counter, and it never needs a compare-exchange.
    std::atomic<bool> active{true};

    // in bytes, 0 = no limit
    size_t limit_size = 0;
    size_t min_tokens = 0;

    // entries idle for longer than this are dropped, 0 = never expire
    int64_t ttl_seconds = 0;

    // pending-write byte budget; the queued entries share the RAM cache buffers
    // (shared_ptr, no extra memory), but a slow disk must not let the queue
    // grow without bound and pin those buffers forever. 0 = no limit
    size_t queue_bytes_max = 0;

    std::vector<server_disk_cache_entry> entries;
    size_t total_size = 0;

    // how many live paths occupy space the ledger cannot size. Not part of
    // `total_size`, which only ever holds real figures: these bytes are real but
    // unmeasured, so admission is denied while any of them is present rather than
    // treating them as zero and over-filling the directory
    size_t n_size_unknown_paths = 0;

    mutable std::mutex      mtx;
    std::condition_variable cv;
    std::thread             th;
    bool                    stop_flag = false;
    std::deque<server_disk_cache_pending> queue;
    size_t                  queue_bytes = 0;

    // monotone sequence of entry state changes, handed to the prefix directory
    uint64_t                event_epoch = 0;

    // outcome of the last claim_path_locked() call, read right after it returns.
    // only exists to avoid allocating a result object on the worker's hot path
    disk_cache_path_owner  path_owner_scratch = disk_cache_path_owner::free;

    // Queued or being-written entries, keyed by hash, so store() and mirror() do
    // not enqueue the same prompt twice.
    //
    // The value is the *complete* token/media identity, owned here rather than
    // pointing into `queue`: the worker pops an element before it has finished
    // writing it, so a pointer into the deque would dangle exactly when the check
    // matters. A hash alone cannot tell "the same prompt is already on its way"
    // from "a different prompt wants the same path", and the two need opposite
    // answers.
    //
    // Only token ids and media metadata are kept - no KV buffers. The size is
    // bounded by the write queue's own entry limit.
    struct inflight_identity {
        llama_tokens              tokens;
        std::vector<uint32_t>     media_indices;
        std::vector<uint32_t>     media_token_counts;
        std::vector<std::string>  media_ids;

        disk_cache_prompt_identity view() const {
            disk_cache_prompt_identity id;
            id.tokens             = &tokens;
            id.media_indices      = &media_indices;
            id.media_token_counts = &media_token_counts;
            id.media_ids          = &media_ids;
            return id;
        }
    };

public:
    // Metadata-only admission check for a prefix that is about to have its full
    // target/draft sequence state exported.
    //
    // The export is the expensive part: sizing the state, allocating buffers in
    // the hundreds of MiB, copying both contexts out. Everything the disk layer
    // already knows - a committed entry with the same identity, a committed entry
    // that covers this prefix cheaply, the same identity already queued, a hash
    // held by a different identity - can be answered from token/media metadata
    // alone, before any of that.
    //
    // Read-only and synchronous. It never touches `entries`, `inflight`, the byte
    // ledger, the manifest or the prefix directory, and it takes no KV state. A
    // result of `export_needed` is not permission to skip the real checks later:
    // store() and the worker still decide, and this call says nothing about the
    // quota, the final buffer size or whether a file will still be there in a
    // moment.
    enum class preflight_result {
        export_needed,          // nothing here says the export is pointless
        identical_committed,    // a reusable entry with this very identity exists
        covered_by_committed,   // a reusable entry covers this prefix cheaply
        identical_inflight,     // this very identity is queued or being written
        hash_path_conflict,     // the path is spoken for by a different identity
        invalid_identity,       // no usable metadata identity could be built
        cache_unavailable,      // the disk tier is not active
    };

    // `tokens` is the prefix itself, not the whole request: the caller passes the
    // first K tokens and their media description
    preflight_result preflight_materialize(const server_tokens & prefix) const;

private:
    std::unordered_map<uint64_t, inflight_identity> inflight;

    // how this tier writes the payload of a new entry. `none` keeps v8: every
    // segment verbatim and no segment table, which is the layout the previous
    // releases produce anywhere. anything else writes v9, where each independent
    // carrier is encoded on its own
    disk_cache_codec_mode codec_mode = disk_cache_codec_mode::none;

    // what a hash already in flight means for a write that wants the same path.
    // `duplicate` is the normal case; `conflict` means the path is spoken for by a
    // different prompt and the write must be refused, never merged and never
    // written over the other one
    enum class inflight_verdict {
        free,
        duplicate,
        conflict,
    };

    // must be called with mtx held
    inflight_verdict inflight_check_locked(uint64_t hash, const disk_cache_prompt_identity & wanted) const;

    // must be called with mtx held. copies the identity out of `pending`'s own
    // arrays, so nothing survives by reference
    void inflight_add_locked(uint64_t hash, const server_disk_cache_pending & pending);

    // must be called with mtx held
    void inflight_erase_locked(uint64_t hash);

    // Release the bookkeeping for a queued write that will not happen: the byte budget
    // it held and its in-flight identity slot. mtx must be held.
    //
    // One function for this, because it is called from more than one place and each of
    // them has to give back exactly the same two things - a missed release leaks the
    // queue budget and leaves an identity claimed, which silently blocks a later write
    // of the same prompt.
    void cancel_pending_locked(const server_disk_cache_pending & pending);

    // what the committed entries already say about a write for `hash`.
    //
    // Shared by the preflight (which only needs to know whether exporting the
    // state is worth it) and by store() (which acts on the answer). Keeping it in
    // one place means the two agree on what "already stored" and "path conflict"
    // mean - a second copy of this rule is exactly how the queued and committed
    // halves drifted apart before.
    //
    // must be called with mtx held
    enum class committed_verdict {
        none,       // no usable entry holds this path
        identical,  // a reusable entry with the same identity
        conflict,   // a reusable entry with a different identity
        held,       // only a quarantined entry holds it; the worker resolves the path
    };

    committed_verdict committed_check_locked(uint64_t hash, const disk_cache_prompt_identity & wanted, std::string * holder) const;

    // delete a broken entry (file + index + manifest); must not be called with mtx held
    void drop_entry(const std::string & path);

    // the single deletion primitive: reports whether the bytes are really gone.
    // it does not touch `entries`, `total_size` or the manifest
    disk_cache_remove_status remove_file(const std::string & path) const;

    // resolve the ownership of the path a write of `wanted` (hash `hash`) would
    // use; mtx must be held. a quarantined holder is reclaimed here when the file
    // can really be removed, because a quarantined entry must not occupy a path
    // that a valid write needs
    disk_cache_path_owner claim_path_locked(uint64_t hash, const std::string & path,
            const disk_cache_prompt_identity & wanted, const char * what);
    // Keep a file that could not be deleted out of the lookup path without
    // pretending its bytes were freed; mtx must be held.
    //
    // Returns whether the exclusion was *recorded*, not merely applied. The in-memory
    // flag alone is not enough: it is lost when the process ends, and the next startup
    // only sees that a file exists - the header check proves the bytes are a snapshot,
    // never that this path is one the cache should serve. So the name has to reach the
    // quarantine record, and the caller has to know whether it did.
    //
    // A `committed` result means the entry is excluded now and will still be excluded
    // after a restart. A `failed` result means the exclusion exists only in this
    // process, which the caller must treat as a fault rather than a normal outcome.
    disk_cache_commit_status quarantine_entry(server_disk_cache_entry & e, const char * why);

    // Put the disk tier into the conservative state it has to be in once something the
    // cache relies on could not be recorded. Idempotent; mtx must be held.
    //
    // This is the difference between a flag and a policy: setting
    // `quarantine_untrusted` says the state is unknown, and this function is what makes
    // the rest of the cache act on that instead of continuing as though nothing
    // happened.
    void disable_disk_tier_locked(const char * why);

    // True when the saved entry is still usable and the quarantine record is intact.
    bool disk_tier_usable_locked() const;
    // notify the prefix directory; mtx must be held
    void notify_committed(const server_disk_cache_entry & e);
    void notify_invalidated(const std::string & path);

    // serialize and atomically commit an entry. `path_owned` tells the writer that
    // the target name has already been resolved as unowned under the cache lock,
    // which is the only situation in which an existing file there may be replaced
    // Writes the entry and reports how it finished. The caller owns every ledger
    // decision; this function only touches bytes and the filesystem.
    //
    // On `committed` (and only then) `path_out` and `size_out` hold the final path and
    // its measured size. On `failed_orphaned` the result carries the path and the best
    // known byte count so the caller can quarantine it under the lock.
    disk_cache_write_result write_entry(const server_disk_cache_pending & pending,
            std::string & path_out, size_t & size_out, bool path_owned = false);
    // read, validate and decode one entry file. every checksum and the
    // packed<->index consistency are verified here, before the caller restores
    // anything into the contexts
    disk_cache_read_status read_entry(const server_disk_cache_entry & e, size_t lcp, server_disk_cache_payload & out);
    // persist the entry's last-used timestamp in the file header
    bool touch_entry(const server_disk_cache_entry & e);
    void scan();
    void enforce_limit();

    // evict unprotected entries until the accounted total fits the limit.
    //
    // `protected_paths` holds the paths this transaction must not lose. it is a set
    // of path *copies* on purpose: `entries` is a vector, so any erase() moves the
    // elements after it and a pointer into the vector would go stale mid-pass
    void enforce_limit(const disk_cache_protected_paths & protected_paths);

    // build the planning input for the current entry set plus, optionally, a
    // provisional entry that is not in `entries` yet; mtx must be held
    std::vector<disk_cache_quota_item> quota_items_locked(const server_disk_cache_entry * keep_new,
            std::vector<std::string> & path_storage) const;
    // remove entries whose prompt is a strict prefix of the given prompt
    // (and whose media chunks are a subset of the given ones): the longer
    // entry serves any lookup the shorter one would
    // `committed_file_bytes` must be the real size of a file that is already on
    // disk and admitted into `entries` - a pre-write estimate may not decide
    // whether an existing snapshot is deleted. `replacement_is_admitted` tells the
    // function whether that entry has already been guaranteed to survive this
    // capacity transaction: only then may it actually delete the shorter
    // snapshots it supersedes. when false the superseded snapshots are kept and
    // only reported.
    //
    // returns how many snapshots were really deleted: the caller uses that to tell
    // "this transaction changed what is recoverable" from "it did not, so the
    // pre-transaction state can simply be restored"
    size_t drop_prefixes(const llama_tokens & ids, const std::vector<uint32_t> & media_indices,
            const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids,
            size_t committed_file_bytes, bool replacement_is_admitted);
    // true when a stored entry already shares all but a short tail of the given
    // prompt **and** could actually be restored for it (media-compatible over the
    // shared prefix, cost guard satisfied), so rewriting would cost more than the
    // prefill the longer prefix saves; mtx must be held
    bool covered_cheaply(const disk_cache_prompt_identity & wanted) const;

    // metadata view of one entry, for the substitution helpers
    static disk_cache_prompt_identity identity_of(const server_disk_cache_entry & e);
    // a candidate that may serve a lookup or stand in for a write: not
    // quarantined and non-empty. it does not imply the file is intact - load()
    // still validates it
    static bool is_reusable_entry(const server_disk_cache_entry & e);
    // remove any stored entry that is a redundant prefix of another stored
    // entry (run once at startup, after scan)
    void dedup_prefixes();
    // drop entries whose last_used is older than the TTL (no-op when disabled)
    void sweep_expired();

    // drop entries idle beyond the TTL. `protected_paths` are never swept: the TTL
    // pass runs before eviction and must not take the entry this transaction is
    // trying to admit
    void sweep_expired(const disk_cache_protected_paths & protected_paths);

    // true when `entry` belongs to the set this transaction must keep
    static bool is_protected(const server_disk_cache_entry & entry,
            const disk_cache_protected_paths & protected_paths);
    // startup index: avoids opening and parsing every entry file when the
    // cache holds many entries; trusted only when the file size matches
    bool load_manifest(std::map<std::string, disk_cache_manifest_meta> & out);
    void save_manifest(); // mtx must be held

    // The quarantine record is a small sidecar, separate from the manifest, listing
    // the entry file names that must not be treated as usable until they are
    // confirmed removed or measured.
    //
    // It is separate because manifest v3 has no field for this, and adding one would
    // change a format whose compatibility the plan requires keeping. A file that
    // exists only because a write failed is exactly the state that must outlive the
    // process that discovered it, so it cannot live only in memory either.
    //
    // The read must happen before the scan decides what is usable; the write only
    // ever adds or removes names, never reclassifies a file as good.
    // Reads the record and reports how it went. `absent` and `io_failure` are kept
    // apart because one lets the directory be used and the other does not.
    disk_cache_quarantine_status load_quarantine(std::set<std::string> & out) const;

    // Writes the record atomically and reports whether it landed. The caller must act
    // on a failure: the record is what keeps an uncommitted path from being read back
    // as a usable snapshot after a restart, so a write that did not land must stop the
    // transaction that would have created that path.
    disk_cache_commit_status save_quarantine(const std::set<std::string> & names) const; // mtx must be held

    disk_cache_commit_status quarantine_paths_add(const std::vector<std::string> & paths); // mtx must be held
    disk_cache_commit_status quarantine_paths_drop(const std::string & path);             // mtx must be held

    // True when the record could not be trusted at startup. The disk tier stays
    // disabled for the rest of the run rather than treating the directory as sound:
    // entries already registered would otherwise be served from paths whose
    // quarantine state is unknown.
    bool quarantine_untrusted = false;

    // entry names (file name only, not the full path) currently marked unusable
    std::set<std::string> quarantined_names;
    void worker();
};

struct server_prompt_cache {
    server_prompt_cache(int32_t limit_size_mib, size_t limit_tokens) {
        this->limit_size   = 1024ull*1024ull*(limit_size_mib < 0 ? 0 : limit_size_mib);
        this->limit_tokens = limit_tokens;
    }

    ~server_prompt_cache() {
        // flush what is still in RAM before the disk tier goes away
        if (disk) {
            for (auto & state : states) {
                spill(state);
            }
            states.clear();
        }
    }

    // attach the second-level on-disk tier
    void init_disk(const std::string & path, const std::string & sig, int32_t limit_size_mib, int32_t min_tokens, int32_t ttl_seconds, int32_t queue_max_mib, disk_cache_codec_mode codec_mode = disk_cache_codec_mode::none);

    std::list<server_prompt_cache_state> states;

    // optional second-level storage
    std::unique_ptr<server_disk_cache> disk;

    // in bytes, 0 = no limit
    size_t limit_size = 0;

    // in tokens, 0 = no limit
    size_t limit_tokens = 0;

    size_t size() const;

    size_t n_tokens() const;

    server_prompt_cache_state * alloc(const server_prompt & prompt, size_t state_size_main, size_t state_size_drft);

    bool load(server_prompt & prompt, const server_tokens & tokens_new, llama_context * ctx_tgt, llama_context * ctx_dft, int32_t id_slot,
            prompt_cache_restore_info & info);

    void update();

    // mirror the RAM entries to the disk tier (called when the server has been idle)
    void flush_to_disk();

    // queue a disk write for the entry that was added last (boundary capture).
    // the usual admission guards apply, so a covered or oversized entry is a no-op
    void mirror_latest(size_t budget);

    // export a prefix as its own disk object without going through the RAM tier.
    //
    // the RAM tier's own admission rules answer a different question - "does this
    // session want to keep the state in memory" - and a prefix that is too large
    // for it is still worth a disk snapshot. the disk tier applies its own
    // identity, dedup, cost and quota checks to the object handed over here
    bool materialize(const server_prompt & prompt, llama_context * ctx_tgt, llama_context * ctx_dft, int32_t id_slot,
            server_prompt_cache_state & out);

private:
    // move an entry that is about to be dropped to the disk tier
    void spill(server_prompt_cache_state & state);
};

// used exclusively by router mode
struct server_task_result_router : server_task_result {
    json data;
    virtual json to_json() override { return data; }
    virtual server_task_result * clone() const override {
        return new server_task_result_router(*this);
    }
};
