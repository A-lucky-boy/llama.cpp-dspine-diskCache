#pragma once

#include "common.h"
#include "llama.h"

#include <string>
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
#include "server-prefix-index.h"
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
    uint64_t n_manifest_fallback    = 0;
    uint64_t n_reject[(size_t) cache_reject_reason::count] = {};
    uint64_t n_enqueue_queued       = 0;
    uint64_t n_enqueue_dedup        = 0; // already stored, already queued or covered by a longer entry
    uint64_t n_enqueue_rejected     = 0; // over the size limit or the queue budget
    uint64_t n_remove_failed        = 0; // files that could not be deleted, cumulative
    uint64_t n_quarantined          = 0; // entries currently excluded from lookups
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
    llama_tokens tokens;
    std::vector<uint32_t> media_indices;
    std::vector<uint32_t> media_token_counts;
    std::vector<std::string> media_ids;
    common_state_data main;
    common_state_data drft;
    std::list<std::shared_ptr<const common_prompt_checkpoint>> checkpoints;
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
    server_disk_cache(const std::string & dir, const std::string & sig, int32_t limit_size_mib, int32_t min_tokens, int32_t ttl_seconds, int32_t queue_max_mib);
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

    bool active = true;

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

    mutable std::mutex      mtx;
    std::condition_variable cv;
    std::thread             th;
    bool                    stop_flag = false;
    std::deque<server_disk_cache_pending> queue;
    size_t                  queue_bytes = 0;

    // monotone sequence of entry state changes, handed to the prefix directory
    uint64_t                event_epoch = 0;

    // hashes of entries that are queued or being written, so store() and
    // mirror() do not enqueue the same prompt twice
    std::unordered_set<uint64_t> inflight;

private:
    // delete a broken entry (file + index + manifest); must not be called with mtx held
    void drop_entry(const std::string & path);

    // the single deletion primitive: reports whether the bytes are really gone.
    // it does not touch `entries`, `total_size` or the manifest
    disk_cache_remove_status remove_file(const std::string & path) const;
    // keep a file that could not be deleted out of the lookup path without
    // pretending its bytes were freed; mtx must be held
    void quarantine_entry(server_disk_cache_entry & e, const char * why);
    // notify the prefix directory; mtx must be held
    void notify_committed(const server_disk_cache_entry & e);
    void notify_invalidated(const std::string & path);

    bool write_entry(const server_disk_cache_pending & pending, std::string & path_out, size_t & size_out);
    // read, validate and decode one entry file. every checksum and the
    // packed<->index consistency are verified here, before the caller restores
    // anything into the contexts
    disk_cache_read_status read_entry(const server_disk_cache_entry & e, size_t lcp, server_disk_cache_payload & out);
    // persist the entry's last-used timestamp in the file header
    bool touch_entry(const server_disk_cache_entry & e);
    void scan();
    void enforce_limit();
    // remove entries whose prompt is a strict prefix of the given prompt
    // (and whose media chunks are a subset of the given ones): the longer
    // entry serves any lookup the shorter one would
    void drop_prefixes(const llama_tokens & ids, const std::vector<uint32_t> & media_indices,
            const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids);
    // true when a stored entry already shares all but a short tail of the given
    // prompt, so rewriting the entry would cost more than the prefill the longer
    // prefix saves; mtx must be held
    bool covered_cheaply(const llama_tokens & ids) const;
    // remove any stored entry that is a redundant prefix of another stored
    // entry (run once at startup, after scan)
    void dedup_prefixes();
    // drop entries whose last_used is older than the TTL (no-op when disabled)
    void sweep_expired();
    // startup index: avoids opening and parsing every entry file when the
    // cache holds many entries; trusted only when the file size matches
    bool load_manifest(std::map<std::string, disk_cache_manifest_meta> & out);
    void save_manifest(); // mtx must be held
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
    void init_disk(const std::string & path, const std::string & sig, int32_t limit_size_mib, int32_t min_tokens, int32_t ttl_seconds, int32_t queue_max_mib);

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
