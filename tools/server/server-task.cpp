#include "server-task.h"

#include "build-info.h"
#include "server-chat.h"
#include "chat.h"
#include "common.h"
#include "json-schema-to-grammar.h"
#include "llama.h"
#include "sampling.h"
#include "speculative.h"
#include "server-common.h"

#include <sstream>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>

//
// task_params
//

json task_params::format_logit_bias(const std::vector<llama_logit_bias> & logit_bias) const {
    json data = json::array();
    for (const auto & lb : logit_bias) {
        data.push_back(json{
            {"bias", lb.bias},
            {"token", lb.token},
        });
    }
    return data;
}

json task_params::to_json(bool only_metrics) const {
    std::vector<std::string> samplers;
    samplers.reserve(sampling.samplers.size());
    for (const auto & sampler : sampling.samplers) {
        samplers.emplace_back(common_sampler_type_to_str(sampler));
    }

    json lora = json::array();
    for (auto & it : this->lora) {
        lora.push_back({{"id", it.first}, {"scale", it.second}});
    }

    if (only_metrics) {
        return json {
            {"seed",                      sampling.seed},
            {"temperature",               sampling.temp},
            {"dynatemp_range",            sampling.dynatemp_range},
            {"dynatemp_exponent",         sampling.dynatemp_exponent},
            {"top_k",                     sampling.top_k},
            {"top_p",                     sampling.top_p},
            {"min_p",                     sampling.min_p},
            {"top_n_sigma",               sampling.top_n_sigma},
            {"xtc_probability",           sampling.xtc_probability},
            {"xtc_threshold",             sampling.xtc_threshold},
            {"typical_p",                 sampling.typ_p},
            {"repeat_last_n",             sampling.penalty_last_n},
            {"repeat_penalty",            sampling.penalty_repeat},
            {"presence_penalty",          sampling.penalty_present},
            {"frequency_penalty",         sampling.penalty_freq},
            {"dry_multiplier",            sampling.dry_multiplier},
            {"dry_base",                  sampling.dry_base},
            {"dry_allowed_length",        sampling.dry_allowed_length},
            {"dry_penalty_last_n",        sampling.dry_penalty_last_n},
            {"mirostat",                  sampling.mirostat},
            {"mirostat_tau",              sampling.mirostat_tau},
            {"mirostat_eta",              sampling.mirostat_eta},
            {"adaptive_target",           sampling.adaptive_target},
            {"adaptive_decay",            sampling.adaptive_decay},
            {"max_tokens",                n_predict},
            {"n_predict",                 n_predict}, // TODO: deduplicate?
            {"n_keep",                    n_keep},
            {"n_discard",                 n_discard},
            {"ignore_eos",                sampling.ignore_eos},
            {"stream",                    stream},
            {"n_probs",                   sampling.n_probs},
            {"min_keep",                  sampling.min_keep},
            {"chat_format",               common_chat_format_name(chat_format)},
            {"reasoning_format",          common_reasoning_format_name(reasoning_format)},
            {"reasoning_in_content",      stream && reasoning_format == COMMON_REASONING_FORMAT_DEEPSEEK_LEGACY},
            {"generation_prompt",         sampling.generation_prompt},
            {"samplers",                  samplers},
            {"speculative.types",         common_speculative_type_name_str(speculative.types)},
            {"timings_per_token",         timings_per_token},
            {"post_sampling_probs",       post_sampling_probs},
            {"backend_sampling",          sampling.backend_sampling},
            {"lora",                      lora},
        };
    }

    auto grammar_triggers = json::array();
    for (const auto & trigger : sampling.grammar_triggers) {
        server_grammar_trigger ct(trigger);
        grammar_triggers.push_back(ct.to_json());
    }

    return json {
        {"seed",                      sampling.seed},
        {"temperature",               sampling.temp},
        {"dynatemp_range",            sampling.dynatemp_range},
        {"dynatemp_exponent",         sampling.dynatemp_exponent},
        {"top_k",                     sampling.top_k},
        {"top_p",                     sampling.top_p},
        {"min_p",                     sampling.min_p},
        {"top_n_sigma",               sampling.top_n_sigma},
        {"xtc_probability",           sampling.xtc_probability},
        {"xtc_threshold",             sampling.xtc_threshold},
        {"typical_p",                 sampling.typ_p},
        {"repeat_last_n",             sampling.penalty_last_n},
        {"repeat_penalty",            sampling.penalty_repeat},
        {"presence_penalty",          sampling.penalty_present},
        {"frequency_penalty",         sampling.penalty_freq},
        {"dry_multiplier",            sampling.dry_multiplier},
        {"dry_base",                  sampling.dry_base},
        {"dry_allowed_length",        sampling.dry_allowed_length},
        {"dry_penalty_last_n",        sampling.dry_penalty_last_n},
        {"dry_sequence_breakers",     sampling.dry_sequence_breakers},
        {"mirostat",                  sampling.mirostat},
        {"mirostat_tau",              sampling.mirostat_tau},
        {"mirostat_eta",              sampling.mirostat_eta},
        {"adaptive_target",           sampling.adaptive_target},
        {"adaptive_decay",            sampling.adaptive_decay},
        {"stop",                      antiprompt},
        {"max_tokens",                n_predict},
        {"n_predict",                 n_predict}, // TODO: deduplicate?
        {"n_keep",                    n_keep},
        {"n_discard",                 n_discard},
        {"ignore_eos",                sampling.ignore_eos},
        {"stream",                    stream},
        {"logit_bias",                format_logit_bias(sampling.logit_bias)},
        {"n_probs",                   sampling.n_probs},
        {"min_keep",                  sampling.min_keep},
        {"grammar",                   common_grammar_value(sampling.grammar)},
        {"grammar_lazy",              sampling.grammar_lazy},
        {"grammar_triggers",          grammar_triggers},
        {"preserved_tokens",          sampling.preserved_tokens},
        {"chat_format",               common_chat_format_name(chat_format)},
        {"reasoning_format",          common_reasoning_format_name(reasoning_format)},
        {"reasoning_in_content",      stream && reasoning_format == COMMON_REASONING_FORMAT_DEEPSEEK_LEGACY},
        {"generation_prompt",         sampling.generation_prompt},
        {"samplers",                  samplers},
        {"speculative.types",         common_speculative_type_name_str(speculative.types)},
        {"timings_per_token",         timings_per_token},
        {"post_sampling_probs",       post_sampling_probs},
        {"backend_sampling",          sampling.backend_sampling},
        {"lora",                      lora},
    };
}

//
// task_result_state
//
task_result_state::task_result_state(common_chat_session session)
    : chat_session(std::move(session))
    , chat_msg(chat_session.msg())
    , oai_resp_id("resp_" + random_string())
    , oai_resp_reasoning_id("rs_" + random_string())
    , oai_resp_message_id("msg_" + random_string()) {
}

common_chat_msg task_result_state::update_chat_msg(
        const common_chat_input & added,
        bool is_partial,
        std::vector<common_chat_msg_diff> & diffs,
        bool filter_tool_calls) {
    auto msg_prv_copy = chat_msg;
    auto new_msg = is_partial ? chat_session.feed(added) : chat_session.finish(added);
    if (!new_msg.empty()) {
        new_msg.set_tool_call_ids(generated_tool_call_ids, gen_tool_call_id);
        chat_msg = new_msg;
        auto all_diffs = common_chat_msg_diff::compute_diffs(msg_prv_copy, chat_msg);

        if (!filter_tool_calls) {
            diffs = std::move(all_diffs);
        } else {
            for (auto & d : all_diffs) {
                // If this is a new type of delta, flush all currently pending tool call names
                for (size_t i = 0; i < chat_msg.tool_calls.size(); ++i) {
                    if (sent_tool_call_names.count(i) || chat_msg.tool_calls[i].name.empty()) {
                        continue;
                    }
                    if (d.tool_call_index != i || !d.tool_call_delta.arguments.empty()) {
                        common_chat_msg_diff header;
                        header.tool_call_index      = i;
                        header.tool_call_delta.id   = chat_msg.tool_calls[i].id;
                        header.tool_call_delta.name = chat_msg.tool_calls[i].name;
                        diffs.push_back(std::move(header));
                        sent_tool_call_names.insert(i);
                    }
                }

                if (d.tool_call_index == std::string::npos) {
                    diffs.push_back(std::move(d));
                } else {
                    size_t i = d.tool_call_index;
                    if (sent_tool_call_names.count(i)) {
                        if (!d.tool_call_delta.arguments.empty()) {
                            d.tool_call_delta.name = "";
                            d.tool_call_delta.id   = "";
                            diffs.push_back(std::move(d));
                        }
                    } else {
                        // Not sent yet.
                        if (!d.tool_call_delta.arguments.empty() || !is_partial) {
                            d.tool_call_delta.name = chat_msg.tool_calls[i].name;
                            d.tool_call_delta.id   = chat_msg.tool_calls[i].id;
                            diffs.push_back(std::move(d));
                            sent_tool_call_names.insert(i);
                        } else {
                            // Suppress
                        }
                    }
                }
            }
            // Final check at EOF
            if (!is_partial) {
                for (size_t i = 0; i < chat_msg.tool_calls.size(); ++i) {
                    if (!sent_tool_call_names.count(i) && !chat_msg.tool_calls[i].name.empty()) {
                        common_chat_msg_diff header;
                        header.tool_call_index      = i;
                        header.tool_call_delta.id   = chat_msg.tool_calls[i].id;
                        header.tool_call_delta.name = chat_msg.tool_calls[i].name;
                        diffs.push_back(std::move(header));
                        sent_tool_call_names.insert(i);
                    }
                }
            }
        }
    }
    return chat_msg;
}

//
// result_prompt_progress
//
json result_prompt_progress::to_json() const {
    return json {
        {"total",     total},
        {"cache",     cache},
        {"processed", processed},
        {"time_ms",   time_ms},
    };
}

static inline std::string stop_type_to_str(stop_type type) {
    switch (type) {
        case STOP_TYPE_EOS:   return "eos";
        case STOP_TYPE_WORD:  return "word";
        case STOP_TYPE_LIMIT: return "limit";
        default:              return "none";
    }
}

//
// completion_token_output
//

json completion_token_output::to_json(bool post_sampling_probs) const {
    json probs_for_token = json::array();
    for (const auto & p : probs) {
        std::string txt(p.txt);
        txt.resize(validate_utf8(txt));
        probs_for_token.push_back(json {
            {"id",      p.tok},
            {"token",   txt},
            {"bytes",   str_to_bytes(p.txt)},
            {
                post_sampling_probs ? "prob" : "logprob",
                post_sampling_probs ? p.prob : logarithm(p.prob)
            },
        });
    }
    return probs_for_token;
}

json completion_token_output::probs_vector_to_json(const std::vector<completion_token_output> & probs, bool post_sampling_probs) {
    json out = json::array();
    for (const auto & p : probs) {
        std::string txt(p.text_to_send);
        txt.resize(validate_utf8(txt));
        out.push_back(json {
            {"id",           p.tok},
            {"token",        txt},
            {"bytes",        str_to_bytes(p.text_to_send)},
            {
                post_sampling_probs ? "prob" : "logprob",
                post_sampling_probs ? p.prob : logarithm(p.prob)
            },
            {
                post_sampling_probs ? "top_probs" : "top_logprobs",
                p.to_json(post_sampling_probs)
            },
        });
    }
    return out;
}

float completion_token_output::logarithm(float x) {
    // the JSON library converts -inf to null, so we need to prevent that
    return x == 0.0f ? std::numeric_limits<float>::lowest() : std::log(x);
}

std::vector<unsigned char> completion_token_output::str_to_bytes(const std::string & str) {
    std::vector<unsigned char> bytes;
    for (unsigned char c : str) {
        bytes.push_back(c);
    }
    return bytes;
}

//
// server_task_result_cmpl_final
//
json server_task_result_cmpl_final::to_json() {
    GGML_ASSERT(is_updated && "update() must be called before to_json()");
    switch (res_type) {
        case TASK_RESPONSE_TYPE_NONE:
            return to_json_non_oaicompat();
        case TASK_RESPONSE_TYPE_OAI_CMPL:
            return to_json_oaicompat();
        case TASK_RESPONSE_TYPE_OAI_CHAT:
            return stream ? to_json_oaicompat_chat_stream() : to_json_oaicompat_chat();
        case TASK_RESPONSE_TYPE_OAI_RESP:
            return stream ? to_json_oaicompat_resp_stream() : to_json_oaicompat_resp();
        case TASK_RESPONSE_TYPE_OAI_ASR:
            return to_json_oaicompat_asr();
        case TASK_RESPONSE_TYPE_ANTHROPIC:
            return stream ? to_json_anthropic_stream() : to_json_anthropic();
        default:
            GGML_ASSERT(false && "Invalid task_response_type");
    }
}

json server_task_result_cmpl_final::to_json_non_oaicompat() {
    json res = json {
        {"index",               index},
        {"content",             content.text},
        {"tokens",              tokens},
        {"id_slot",             id_slot},
        {"stop",                true},
        {"model",               oaicompat_model},
        {"tokens_predicted",    n_decoded},
        {"tokens_evaluated",    n_prompt_tokens},
        {"generation_settings", generation_params.to_json()},
        {"prompt",              prompt},
        {"has_new_line",        has_new_line},
        {"truncated",           truncated},
        {"stop_type",           stop_type_to_str(stop)},
        {"stopping_word",       stopping_word},
        {"tokens_cached",       n_tokens_cached},
        {"timings",             stats.to_json()},
    };
    if (!stream && !probs_output.empty()) {
        res["completion_probabilities"] = completion_token_output::probs_vector_to_json(probs_output, post_sampling_probs);
    }
    return response_fields.empty() ? res : json_get_nested_values(response_fields, res);
}

json server_task_result_cmpl_final::usage_json_oaicompat() {
    return json {
        {"completion_tokens", n_decoded},
        {"prompt_tokens",     n_prompt_tokens},
        {"total_tokens",      n_decoded + n_prompt_tokens},
        {"prompt_tokens_details", json { {"cached_tokens", n_prompt_tokens_cache} }},
    };
}

json server_task_result_cmpl_final::to_json_oaicompat() {
    std::time_t t = std::time(0);
    json logprobs = json(nullptr); // OAI default to null
    if (!stream && probs_output.size() > 0) {
        logprobs = json{
            {"content", completion_token_output::probs_vector_to_json(probs_output, post_sampling_probs)},
        };
    }
    json finish_reason = "length";
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        finish_reason = "stop";
    }
    json res = json {
        {"choices",            json::array({
            json{
                {"text",          content.text},
                {"index",         index},
                {"logprobs",      logprobs},
                {"finish_reason", finish_reason},
            }
        })},
        {"created",            t},
        {"model",              oaicompat_model},
        {"system_fingerprint", std::string(llama_build_info())},
        {"object",             "text_completion"},
        {"usage",              usage_json_oaicompat()},
        {"id", oaicompat_cmpl_id}
    };

    // extra fields for debugging purposes
    if (verbose) {
        res["__verbose"] = to_json_non_oaicompat();
    }
    if (stats.is_set()) {
        res["timings"] = stats.to_json();
    }

    return res;
}

json server_task_result_cmpl_final::to_json_oaicompat_chat() {
    std::string finish_reason = "length";
    common_chat_msg msg;
    if (!oaicompat_msg.empty()) {
        msg = oaicompat_msg;
    } else {
        msg.role = "assistant";
        msg.content = content.text;
    }
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        finish_reason = msg.tool_calls.empty() ? "stop" : "tool_calls";
    }

    json choice {
        {"finish_reason", finish_reason},
        {"index", index},
        {"message", msg.to_json_oaicompat()},
    };

    if (!stream && probs_output.size() > 0) {
        choice["logprobs"] = json{
            {"content", completion_token_output::probs_vector_to_json(probs_output, post_sampling_probs)},
        };
    }

    std::time_t t = std::time(0);

    json res = json {
        {"choices",            json::array({choice})},
        {"created",            t},
        {"model",              oaicompat_model},
        {"system_fingerprint", std::string(llama_build_info())},
        {"object",             "chat.completion"},
        {"usage",              usage_json_oaicompat()},
        {"id", oaicompat_cmpl_id}
    };

    // extra fields for debugging purposes
    if (verbose) {
        res["__verbose"] = to_json_non_oaicompat();
    }
    if (stats.is_set()) {
        res["timings"] = stats.to_json();
    }

    return res;
}

json server_task_result_cmpl_final::to_json_oaicompat_chat_stream() {
    std::time_t t = std::time(0);
    std::string finish_reason = "length";
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        finish_reason = oaicompat_msg.tool_calls.empty() ? "stop" : "tool_calls";
    }

    json deltas = json::array();
    for (const auto & diff : oaicompat_msg_diffs) {
        deltas.push_back({
            {"choices", json::array({
                json {
                    {"finish_reason", nullptr},
                    {"index", index},
                    {"delta", server_chat_msg_diff_to_json_oaicompat(diff)},
                },
            })},
            {"created", t},
            {"id", oaicompat_cmpl_id},
            {"model", oaicompat_model},
            {"system_fingerprint", std::string(llama_build_info())},
            {"object", "chat.completion.chunk"},
        });
    }

    deltas.push_back({
        {"choices", json::array({
            json {
                {"finish_reason", finish_reason},
                {"index", index},
                {"delta", json::object()},
            },
        })},
        {"created",            t},
        {"id",                 oaicompat_cmpl_id},
        {"model",              oaicompat_model},
        {"system_fingerprint", std::string(llama_build_info())},
        {"object",             "chat.completion.chunk"},
    });

    if (include_usage) {
        // OpenAI API spec for chat.completion.chunks specifies an empty `choices` array for the last chunk when including usage
        // https://platform.openai.com/docs/api-reference/chat_streaming/streaming#chat_streaming/streaming-choices
        deltas.push_back({
            {"choices", json::array()},
            {"created",            t},
            {"id",                 oaicompat_cmpl_id},
            {"model",              oaicompat_model},
            {"system_fingerprint", std::string(llama_build_info())},
            {"object",             "chat.completion.chunk"},
            {"usage",              usage_json_oaicompat()},
        });
    }

    if (stats.is_set()) {
        deltas.back()["timings"] = stats.to_json();
    }

    // extra fields for debugging purposes
    if (verbose && !deltas.empty()) {
        deltas.front()["__verbose"] = to_json_non_oaicompat();
    }

    return deltas;
}

json server_task_result_cmpl_final::to_json_oaicompat_resp() {
    common_chat_msg msg;
    if (!oaicompat_msg.empty()) {
        msg = oaicompat_msg;
    } else {
        msg.role = "assistant";
        msg.content = content.text;
    }

    std::vector<json> output;

    if (msg.reasoning_content != "") {
        output.push_back(json {
            {"id",      "rs_" + random_string()},
            {"summary", json::array()},
            {"type",    "reasoning"},
            {"content", json::array({ json {
                {"text", msg.reasoning_content},
                {"type", "reasoning_text"},
            }})},
            {"encrypted_content", ""},
            {"status",            "completed"},
        });
    }

    if (msg.content != "") {
        output.push_back(json {
            {"content", json::array({ json {
                {"type",        "output_text"},
                {"annotations", json::array()},
                {"logprobs",    json::array()},
                {"text",        msg.content},
            }})},
            {"id",     "msg_" + random_string()},
            {"role",   msg.role},
            {"status", "completed"},
            {"type",   "message"},
        });
    }

    for (const common_chat_tool_call & tool_call : oaicompat_msg.tool_calls) {
        output.push_back(json {
            {"id",        "fc_" + tool_call.id},
            {"type",      "function_call"},
            {"status",    "completed"},
            {"arguments", tool_call.arguments},
            {"call_id",   "call_" + tool_call.id},
            {"name",      tool_call.name},
        });
    }

    std::time_t t = std::time(0);
    json res = {
        {"completed_at", t},
        {"created_at",   t},
        {"id",           oai_resp_id},
        {"model",        oaicompat_model},
        {"object",       "response"},
        {"output",       output},
        {"status",       "completed"},
        {"usage",        json {
            {"input_tokens",  n_prompt_tokens},
            {"output_tokens", n_decoded},
            {"total_tokens",  n_decoded + n_prompt_tokens},
            {"input_tokens_details", json { {"cached_tokens", n_prompt_tokens_cache} }},
        }},
    };

    return res;
}

json server_task_result_cmpl_final::to_json_oaicompat_resp_stream() {
    std::vector<json> server_sent_events;
    std::vector<json> output;
    size_t output_index = 0;

    if (oaicompat_msg.reasoning_content != "") {
        const size_t item_output_index = output_index++;
        const json output_item = json {
            {"id",      oai_resp_reasoning_id},
            {"summary", json::array()},
            {"type",    "reasoning"},
            {"content", json::array({ json {
                {"text", oaicompat_msg.reasoning_content},
                {"type", "reasoning_text"},
            }})},
            {"encrypted_content", ""},
        };

        server_sent_events.push_back(json {
            {"event", "response.output_item.done"},
            {"data", json {
                {"type", "response.output_item.done"},
                {"output_index", item_output_index},
                {"item", output_item}
            }}
        });
        output.push_back(output_item);
    }

    if (oaicompat_msg.content != "") {
        const size_t item_output_index = output_index++;
        server_sent_events.push_back(json {
            {"event", "response.output_text.done"},
            {"data", json {
                {"type",    "response.output_text.done"},
                {"item_id", oai_resp_message_id},
                {"output_index", item_output_index},
                {"content_index", 0},
                {"text",    oaicompat_msg.content}
            }}
        });

        const json content_part = {
            {"type",        "output_text"},
            {"annotations", json::array()},
            {"logprobs",    json::array()},
            {"text",        oaicompat_msg.content}
        };

        server_sent_events.push_back(json {
            {"event", "response.content_part.done"},
            {"data", json {
                {"type",    "response.content_part.done"},
                {"item_id", oai_resp_message_id},
                {"output_index", item_output_index},
                {"content_index", 0},
                {"part",    content_part}
            }}
        });
        const json output_item = {
            {"type",    "message"},
            {"status",  "completed"},
            {"id",      oai_resp_message_id},
            {"content", json::array({content_part})},
            {"role",    "assistant"}
        };

        server_sent_events.push_back(json {
            {"event", "response.output_item.done"},
            {"data", json {
                {"type", "response.output_item.done"},
                {"output_index", item_output_index},
                {"item", output_item}
            }}
        });
        output.push_back(output_item);
    }

    for (const common_chat_tool_call & tool_call : oaicompat_msg.tool_calls) {
        const size_t item_output_index = output_index++;
        const json output_item = {
            {"id",        "fc_" + tool_call.id},
            {"type",      "function_call"},
            {"status",    "completed"},
            {"arguments", tool_call.arguments},
            {"call_id",   "call_" + tool_call.id},
            {"name",      tool_call.name}
        };
        server_sent_events.push_back(json {
            {"event", "response.output_item.done"},
            {"data", json {
                {"type", "response.output_item.done"},
                {"output_index", item_output_index},
                {"item", output_item}
            }}
        });
        output.push_back(output_item);
    }

    std::time_t t = std::time(0);
    server_sent_events.push_back(json {
        {"event", "response.completed"},
        {"data", json {
            {"type", "response.completed"},
            {"response", json {
                {"id",         oai_resp_id},
                {"object",     "response"},
                {"created_at", t},
                {"status",     "completed"},
                {"model",      oaicompat_model},
                {"output",     output},
                {"usage",      json {
                    {"input_tokens",  n_prompt_tokens},
                    {"output_tokens", n_decoded},
                    {"total_tokens",  n_decoded + n_prompt_tokens},
                    {"input_tokens_details", json { {"cached_tokens", n_prompt_tokens_cache} }},
                }}
            }},
        }}
    });

    if (stats.is_set()) {
        server_sent_events.back().at("data")["timings"] = stats.to_json();
    }

    return server_sent_events;
}

json server_task_result_cmpl_final::to_json_oaicompat_asr() {
    json event = json {
        {"type",  "transcript.text.done"},
        {"text",  oaicompat_msg.content},
        {"usage", json {
            {"type",         "tokens"},
            {"input_tokens",  n_prompt_tokens},
            {"output_tokens", n_decoded},
            {"total_tokens",  n_decoded + n_prompt_tokens},
            {"input_tokens_details", json { {"cached_tokens", n_prompt_tokens_cache} }},
        }},
    };
    return event;
}

json server_task_result_cmpl_final::to_json_anthropic() {
    std::string stop_reason = "max_tokens";
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        stop_reason = oaicompat_msg.tool_calls.empty() ? "end_turn" : "tool_use";
    }

    json content_blocks = json::array();

    common_chat_msg msg;
    if (!oaicompat_msg.empty()) {
        msg = oaicompat_msg;
    } else {
        msg.role = "assistant";
        msg.content = content.text;
    }

    // thinking block comes first (Anthropic extended thinking format)
    if (!msg.reasoning_content.empty()) {
        content_blocks.push_back({
            {"type", "thinking"},
            {"thinking", msg.reasoning_content},
            {"signature", ""}  // empty signature for local models (no cryptographic verification)
        });
    }

    if (!msg.content.empty()) {
        content_blocks.push_back({
            {"type", "text"},
            {"text", msg.content}
        });
    }

    for (const auto & tool_call : msg.tool_calls) {
        json tool_use_block = {
            {"type", "tool_use"},
            {"id", tool_call.id},
            {"name", tool_call.name}
        };

        try {
            tool_use_block["input"] = json::parse(tool_call.arguments);
        } catch (const std::exception &) {
            tool_use_block["input"] = json::object();
        }

        content_blocks.push_back(tool_use_block);
    }

    json res = {
        {"id", oaicompat_cmpl_id},
        {"type", "message"},
        {"role", "assistant"},
        {"content", content_blocks},
        {"model", oaicompat_model},
        {"stop_reason", stop_reason},
        {"stop_sequence", stopping_word.empty() ? nullptr : json(stopping_word)},
        {"usage", {
            {"cache_read_input_tokens", n_prompt_tokens_cache},
            {"input_tokens", n_prompt_tokens - n_prompt_tokens_cache},
            {"output_tokens", n_decoded}
        }}
    };

    return res;
}

json server_task_result_cmpl_final::to_json_anthropic_stream() {
    json events = json::array();

    std::string stop_reason = "max_tokens";
    if (stop == STOP_TYPE_WORD || stop == STOP_TYPE_EOS) {
        stop_reason = oaicompat_msg.tool_calls.empty() ? "end_turn" : "tool_use";
    }

    bool has_thinking = !oaicompat_msg.reasoning_content.empty();
    bool has_text     = !oaicompat_msg.content.empty();
    size_t num_tool_calls = oaicompat_msg.tool_calls.size();

    // content block indices: thinking (0) -> text (0 or 1) -> tool_use (n+)
    size_t thinking_block_index = 0;
    size_t text_block_index     = has_thinking ? 1 : 0;

    bool thinking_block_started = false;
    bool text_block_started     = false;
    std::unordered_set<size_t> tool_calls_started;

    for (const auto & diff : oaicompat_msg_diffs) {
        // handle thinking/reasoning content
        if (!diff.reasoning_content_delta.empty()) {
            if (!thinking_block_started) {
                events.push_back({
                    {"event", "content_block_start"},
                    {"data", {
                        {"type", "content_block_start"},
                        {"index", thinking_block_index},
                        {"content_block", {
                            {"type", "thinking"},
                            {"thinking", ""}
                        }}
                    }}
                });
                thinking_block_started = true;
            }

            events.push_back({
                {"event", "content_block_delta"},
                {"data", {
                    {"type", "content_block_delta"},
                    {"index", thinking_block_index},
                    {"delta", {
                        {"type", "thinking_delta"},
                        {"thinking", diff.reasoning_content_delta}
                    }}
                }}
            });
        }

        // handle regular text content
        if (!diff.content_delta.empty()) {
            if (!text_block_started) {
                events.push_back({
                    {"event", "content_block_start"},
                    {"data", {
                        {"type", "content_block_start"},
                        {"index", text_block_index},
                        {"content_block", {
                            {"type", "text"},
                            {"text", ""}
                        }}
                    }}
                });
                text_block_started = true;
            }

            events.push_back({
                {"event", "content_block_delta"},
                {"data", {
                    {"type", "content_block_delta"},
                    {"index", text_block_index},
                    {"delta", {
                        {"type", "text_delta"},
                        {"text", diff.content_delta}
                    }}
                }}
            });
        }

        // handle tool calls
        if (diff.tool_call_index != std::string::npos) {
            size_t content_block_index = (has_thinking ? 1 : 0) + (has_text ? 1 : 0) + diff.tool_call_index;

            if (tool_calls_started.find(diff.tool_call_index) == tool_calls_started.end()) {
                const auto & full_tool_call = oaicompat_msg.tool_calls[diff.tool_call_index];

                events.push_back({
                    {"event", "content_block_start"},
                    {"data", {
                        {"type", "content_block_start"},
                        {"index", content_block_index},
                        {"content_block", {
                            {"type", "tool_use"},
                            {"id", full_tool_call.id},
                            {"name", full_tool_call.name}
                        }}
                    }}
                });
                tool_calls_started.insert(diff.tool_call_index);
            }

            if (!diff.tool_call_delta.arguments.empty()) {
                events.push_back({
                    {"event", "content_block_delta"},
                    {"data", {
                        {"type", "content_block_delta"},
                        {"index", content_block_index},
                        {"delta", {
                            {"type", "input_json_delta"},
                            {"partial_json", diff.tool_call_delta.arguments}
                        }}
                    }}
                });
            }
        }
    }

    // close content blocks in order
    if (has_thinking) {
        // Anthropic API requires a signature_delta before closing thinking blocks
        // We use an empty signature since we can't generate a cryptographic signature for local models
        events.push_back({
            {"event", "content_block_delta"},
            {"data", {
                {"type", "content_block_delta"},
                {"index", thinking_block_index},
                {"delta", {
                    {"type", "signature_delta"},
                    {"signature", ""}
                }}
            }}
        });
        events.push_back({
            {"event", "content_block_stop"},
            {"data", {
                {"type", "content_block_stop"},
                {"index", thinking_block_index}
            }}
        });
    }

    if (has_text) {
        events.push_back({
            {"event", "content_block_stop"},
            {"data", {
                {"type", "content_block_stop"},
                {"index", text_block_index}
            }}
        });
    }

    for (size_t i = 0; i < num_tool_calls; i++) {
        size_t content_block_index = (has_thinking ? 1 : 0) + (has_text ? 1 : 0) + i;
        events.push_back({
            {"event", "content_block_stop"},
            {"data", {
                {"type", "content_block_stop"},
                {"index", content_block_index}
            }}
        });
    }

    events.push_back({
        {"event", "message_delta"},
        {"data", {
            {"type", "message_delta"},
            {"delta", {
                {"stop_reason", stop_reason},
                {"stop_sequence", stopping_word.empty() ? nullptr : json(stopping_word)}
            }},
            {"usage", {
                {"output_tokens", n_decoded}
            }}
        }}
    });

    events.push_back({
        {"event", "message_stop"},
        {"data", {
            {"type", "message_stop"}
        }}
    });

    return events;
}

//
// server_task_result_cmpl_partial
//
void server_task_result_cmpl_partial::update(task_result_state & state) {
    is_updated = true;
    if (is_begin) {
        return; // begin marker only flushes headers, skip parsing
    }
    state.update_chat_msg(content, true, oaicompat_msg_diffs);

    // Copy current state for use in to_json_*() (reflects state BEFORE this chunk)
    thinking_block_started = state.thinking_block_started;
    text_block_started     = state.text_block_started;

    oai_resp_created       = state.oai_resp_created;
    oai_resp_id            = state.oai_resp_id;
    oai_resp_reasoning_id  = state.oai_resp_reasoning_id;
    oai_resp_message_id    = state.oai_resp_message_id;
    oai_resp_fc_id         = state.oai_resp_fc_id;

    // track if the accumulated message has any reasoning content
    anthropic_has_reasoning = !state.chat_msg.reasoning_content.empty();

    if (res_type == TASK_RESPONSE_TYPE_OAI_RESP && !state.oai_resp_created && (is_progress || n_decoded == 1)) {
        state.oai_resp_created = true;
    }

    // Pre-compute state updates based on diffs (for next chunk)
    for (const common_chat_msg_diff & diff : oaicompat_msg_diffs) {
        if (!diff.reasoning_content_delta.empty() && !state.thinking_block_started) {
            state.thinking_block_started = true;
        }
        if (!diff.content_delta.empty() && !state.text_block_started) {
            state.text_block_started = true;
        }
        if (!diff.tool_call_delta.name.empty()) {
            state.oai_resp_fc_id = diff.tool_call_delta.id;
        }
    }
}

json server_task_result_cmpl_partial::to_json() {
    GGML_ASSERT(is_updated && "update() must be called before to_json()");
    if (is_begin) {
        return nullptr; // simply signal to HTTP handler to send the headers and status code
    }
    switch (res_type) {
        case TASK_RESPONSE_TYPE_NONE:
            return to_json_non_oaicompat();
        case TASK_RESPONSE_TYPE_OAI_CMPL:
            return to_json_oaicompat();
        case TASK_RESPONSE_TYPE_OAI_CHAT:
            return to_json_oaicompat_chat();
        case TASK_RESPONSE_TYPE_OAI_RESP:
            return to_json_oaicompat_resp();
        case TASK_RESPONSE_TYPE_OAI_ASR:
            return to_json_oaicompat_asr();
        case TASK_RESPONSE_TYPE_ANTHROPIC:
            return to_json_anthropic();
        default:
            GGML_ASSERT(false && "Invalid task_response_type");
    }
}

json server_task_result_cmpl_partial::to_json_non_oaicompat() {
    // non-OAI-compat JSON
    json res = json {
        {"index",            index},
        {"content",          content.text},
        {"tokens",           tokens},
        {"stop",             false},
        {"id_slot",          id_slot},
        {"tokens_predicted", n_decoded},
        {"tokens_evaluated", n_prompt_tokens},
    };
    // populate the timings object when needed (usually for the last response or with timings_per_token enabled)
    if (stats.is_set()) {
        res["timings"] = stats.to_json();
    }
    if (is_progress) {
        res["prompt_progress"] = progress.to_json();
    }
    if (!prob_output.probs.empty()) {
        res["completion_probabilities"] = completion_token_output::probs_vector_to_json({prob_output}, post_sampling_probs);
    }
    return res;
}

json server_task_result_cmpl_partial::to_json_oaicompat() {
    std::time_t t = std::time(0);
    json logprobs = json(nullptr); // OAI default to null
    if (prob_output.probs.size() > 0) {
        logprobs = json{
            {"content", completion_token_output::probs_vector_to_json({prob_output}, post_sampling_probs)},
        };
    }
    json res = json {
        {"choices",            json::array({
            json{
                {"text",          content.text},
                {"index",         index},
                {"logprobs",      logprobs},
                {"finish_reason", nullptr},
            }
        })},
        {"created",            t},
        {"model",              oaicompat_model},
        {"system_fingerprint", std::string(llama_build_info())},
        {"object",             "text_completion"},
        {"id",                 oaicompat_cmpl_id}
    };

    // extra fields for debugging purposes
    if (verbose) {
        res["__verbose"] = to_json_non_oaicompat();
    }
    if (stats.is_set()) {
        res["timings"] = stats.to_json();
    }
    if (is_progress) {
        res["prompt_progress"] = progress.to_json();
    }

    return res;
}

json server_task_result_cmpl_partial::to_json_oaicompat_chat() {
    bool first = n_decoded == 1;
    std::time_t t = std::time(0);
    json choices;

    std::vector<json> deltas;
    auto add_delta = [&](const json & delta) {
        deltas.push_back({
            {"choices", json::array({
                json {
                    {"finish_reason", nullptr},
                    {"index", index},
                    {"delta", delta},
                },
            })},
            {"created", t},
            {"id", oaicompat_cmpl_id},
            {"model", oaicompat_model},
            {"system_fingerprint", std::string(llama_build_info())},
            {"object", "chat.completion.chunk"},
        });
    };
    // We have to send an initial update to conform to openai behavior
    if (first || is_progress) {
        add_delta({
            {"role", "assistant"},
            {"content", nullptr},
        });
    }

    for (const auto & diff : oaicompat_msg_diffs) {
        add_delta(server_chat_msg_diff_to_json_oaicompat(diff));
    }

    if (!deltas.empty()) {
        auto & last_json = deltas[deltas.size() - 1];
        GGML_ASSERT(last_json.at("choices").size() >= 1);

        if (prob_output.probs.size() > 0) {
            last_json.at("choices").at(0)["logprobs"] = json {
                {"content", completion_token_output::probs_vector_to_json({prob_output}, post_sampling_probs)},
            };
        }

        if (stats.is_set()) {
            last_json["timings"] = stats.to_json();
        }
        if (is_progress) {
            last_json["prompt_progress"] = progress.to_json();
        }
    }

    return deltas;
}

json server_task_result_cmpl_partial::to_json_oaicompat_resp() {
    std::vector<json> events;

    if (!oai_resp_created) {
        events.push_back(json {
            {"event", "response.created"},
            {"data", json {
                {"type", "response.created"},
                {"response", json {
                    {"id",     oai_resp_id},
                    {"object", "response"},
                    {"status", "in_progress"},
                }},
            }},
        });
        events.push_back(json {
            {"event", "response.in_progress"},
            {"data", json {
                {"type", "response.in_progress"},
                {"response", json {
                    {"id",     oai_resp_id},
                    {"object", "response"},
                    {"status", "in_progress"},
                }},
            }},
        });
    } else if (is_progress) {
        events.push_back(json {
            {"event", "response.in_progress"},
            {"data", json {
                {"type", "response.in_progress"},
                {"response", json {
                    {"id",     oai_resp_id},
                    {"object", "response"},
                    {"status", "in_progress"},
                }},
            }},
        });
    }

    for (const common_chat_msg_diff & diff : oaicompat_msg_diffs) {
        if (!diff.reasoning_content_delta.empty()) {
            if (!thinking_block_started) {
                events.push_back(json {
                    {"event", "response.output_item.added"},
                    {"data", json {
                        {"type", "response.output_item.added"},
                        {"output_index", 0},
                        {"item", json {
                            {"id",                oai_resp_reasoning_id},
                            {"summary",           json::array()},
                            {"type",              "reasoning"},
                            {"content",           json::array()},
                            {"encrypted_content", ""},
                            {"status",            "in_progress"},
                        }},
                    }},
                });
                thinking_block_started = true;
            }
            events.push_back(json {
                {"event", "response.reasoning_text.delta"},
                {"data", json {
                    {"type",    "response.reasoning_text.delta"},
                    {"output_index", 0},
                    {"content_index", 0},
                    {"delta",   diff.reasoning_content_delta},
                    {"item_id", oai_resp_reasoning_id},
                }},
            });
        }

        if (!diff.content_delta.empty()) {
            if (!text_block_started) {
                events.push_back(json {
                    {"event", "response.output_item.added"},
                    {"data", json {
                        {"type", "response.output_item.added"},
                        {"output_index", anthropic_has_reasoning ? 1 : 0},
                        {"item", json {
                            {"content", json::array()},
                            {"id",      oai_resp_message_id},
                            {"role",    "assistant"},
                            {"status",  "in_progress"},
                            {"type",    "message"},
                        }},
                    }},
                });
                events.push_back(json {
                    {"event", "response.content_part.added"},
                    {"data", json {
                        {"type",    "response.content_part.added"},
                        {"item_id", oai_resp_message_id},
                        {"output_index", anthropic_has_reasoning ? 1 : 0},
                        {"content_index", 0},
                        {"part", json {
                            {"type", "output_text"},
                            {"text", ""},
                        }},
                    }},
                });
                text_block_started = true;
            }
            events.push_back(json {
                {"event", "response.output_text.delta"},
                {"data", json {
                    {"type",    "response.output_text.delta"},
                    {"item_id", oai_resp_message_id},
                    {"output_index", anthropic_has_reasoning ? 1 : 0},
                    {"content_index", 0},
                    {"delta",   diff.content_delta},
                }},
            });
        }

        if (!diff.tool_call_delta.name.empty()) {
            events.push_back(json {
                {"event", "response.output_item.added"},
                {"data", json {
                    {"type",  "response.output_item.added"},
                    {"output_index", (anthropic_has_reasoning ? 1 : 0) + (text_block_started ? 1 : 0) + diff.tool_call_index},
                    {"item", json {
                        {"id",        "fc_" + diff.tool_call_delta.id},
                        {"arguments", ""},
                        {"call_id",   "call_" + diff.tool_call_delta.id},
                        {"name",      diff.tool_call_delta.name},
                        {"type",      "function_call"},
                        {"status",    "in_progress"},
                    }},
                }},
            });
            oai_resp_fc_id = diff.tool_call_delta.id;
        }

        if (!diff.tool_call_delta.arguments.empty()) {
            events.push_back(json {
                {"event", "response.function_call_arguments.delta"},
                {"data", json {
                    {"type",    "response.function_call_arguments.delta"},
                    {"delta",   diff.tool_call_delta.arguments},
                    {"item_id", "fc_" + oai_resp_fc_id},
                    {"output_index", (anthropic_has_reasoning ? 1 : 0) + (text_block_started ? 1 : 0) + diff.tool_call_index},
                }},
            });
        }
    }

    if (!events.empty()) {
        json & data = events.back().at("data");
        if (stats.is_set()) {
            data["timings"] = stats.to_json();
        }
        if (is_progress) {
            data["prompt_progress"] = progress.to_json();
        }
    }

    return events;
}

json server_task_result_cmpl_partial::to_json_oaicompat_asr() {
    json event = json {
        {"type", "transcript.text.delta"},
        {"delta", content.text},
    };
    return event;
}

json server_task_result_cmpl_partial::to_json_anthropic() {
    json events = json::array();
    bool first = (n_decoded == 1);
    // use member variables to track block state across streaming calls
    // (anthropic_thinking_block_started, anthropic_text_block_started)

    if (first) {
        events.push_back({
            {"event", "message_start"},
            {"data", {
                {"type", "message_start"},
                {"message", {
                    {"id", oaicompat_cmpl_id},
                    {"type", "message"},
                    {"role", "assistant"},
                    {"content", json::array()},
                    {"model", oaicompat_model},
                    {"stop_reason", nullptr},
                    {"stop_sequence", nullptr},
                    {"usage", {
                        {"cache_read_input_tokens", n_prompt_tokens_cache},
                        {"input_tokens", n_prompt_tokens - n_prompt_tokens_cache},
                        {"output_tokens", 0}
                    }}
                }}
            }}
        });
    }

    // content block indices: thinking (0) -> text (0 or 1) -> tool_use (n+)
    size_t thinking_block_index = 0;
    // use anthropic_has_reasoning (set in update()) to know if ANY reasoning was generated
    size_t text_block_index     = anthropic_has_reasoning ? 1 : 0;

    // use local copies of streaming state (copied from task_result_state in update())
    // these reflect the state BEFORE this chunk was processed
    bool thinking_started = thinking_block_started;
    bool text_started     = text_block_started;

    for (const auto & diff : oaicompat_msg_diffs) {
        // handle thinking/reasoning content
        if (!diff.reasoning_content_delta.empty()) {
            if (!thinking_started) {
                events.push_back({
                    {"event", "content_block_start"},
                    {"data", {
                        {"type", "content_block_start"},
                        {"index", thinking_block_index},
                        {"content_block", {
                            {"type", "thinking"},
                            {"thinking", ""}
                        }}
                    }}
                });
                thinking_started = true;
            }

            events.push_back({
                {"event", "content_block_delta"},
                {"data", {
                    {"type", "content_block_delta"},
                    {"index", thinking_block_index},
                    {"delta", {
                        {"type", "thinking_delta"},
                        {"thinking", diff.reasoning_content_delta}
                    }}
                }}
            });
        }

        // handle regular text content
        if (!diff.content_delta.empty()) {
            if (!text_started) {
                events.push_back({
                    {"event", "content_block_start"},
                    {"data", {
                        {"type", "content_block_start"},
                        {"index", text_block_index},
                        {"content_block", {
                            {"type", "text"},
                            {"text", ""}
                        }}
                    }}
                });
                text_started = true;
            }

            events.push_back({
                {"event", "content_block_delta"},
                {"data", {
                    {"type", "content_block_delta"},
                    {"index", text_block_index},
                    {"delta", {
                        {"type", "text_delta"},
                        {"text", diff.content_delta}
                    }}
                }}
            });
        }

        // handle tool calls
        if (diff.tool_call_index != std::string::npos) {
            // use anthropic_has_reasoning for thinking block count (persists across calls)
            size_t content_block_index = (anthropic_has_reasoning ? 1 : 0) + (text_started ? 1 : 0) + diff.tool_call_index;

            if (!diff.tool_call_delta.name.empty()) {
                events.push_back({
                    {"event", "content_block_start"},
                    {"data", {
                        {"type", "content_block_start"},
                        {"index", content_block_index},
                        {"content_block", {
                            {"type", "tool_use"},
                            {"id", diff.tool_call_delta.id},
                            {"name", diff.tool_call_delta.name}
                        }}
                    }}
                });
            }

            if (!diff.tool_call_delta.arguments.empty()) {
                events.push_back({
                    {"event", "content_block_delta"},
                    {"data", {
                        {"type", "content_block_delta"},
                        {"index", content_block_index},
                        {"delta", {
                            {"type", "input_json_delta"},
                            {"partial_json", diff.tool_call_delta.arguments}
                        }}
                    }}
                });
            }
        }
    }

    return events;
}

//
// server_task_result_embd
//
json server_task_result_embd::to_json() {
    return res_type == TASK_RESPONSE_TYPE_OAI_EMBD
        ? to_json_oaicompat()
        : to_json_non_oaicompat();
}

json server_task_result_embd::to_json_non_oaicompat() {
    return json {
        {"index",     index},
        {"embedding", embedding},
    };
}

json server_task_result_embd::to_json_oaicompat() {
    return json {
        {"index",            index},
        {"embedding",        embedding[0]},
        {"tokens_evaluated", n_tokens},
    };
}

//
// server_task_result_rerank
//
json server_task_result_rerank::to_json() {
    return json {
        {"index",            index},
        {"score",            score},
        {"tokens_evaluated", n_tokens},
    };
}

//
// server_task_result_tts
//
json server_task_result_realtime::to_json() {
    return json {
        {"text",          text},
        {"n_samples",     audio.size()},
        {"response_done", response_done},
        {"cancelled",     cancelled},
        {"n_decoded",     n_decoded},
    };
}

json server_task_result_tts::to_json() {
    return json {
        {"sample_rate", sample_rate},
        {"n_bytes",     audio.size()},
        {"final",       final},
    };
}

//
// server_task_result_decision
//
json server_task_result_decision::to_json() {
    return json {
        {"index",            index},
        {"scores",           scores},
        {"tokens_evaluated", n_tokens},
    };
}

//
// server_task_result_error
//
json server_task_result_error::to_json() {
    json res = format_error_response(err_msg, err_type);
    if (err_type == ERROR_TYPE_EXCEED_CONTEXT_SIZE) {
        res["n_prompt_tokens"] = n_prompt_tokens;
        res["n_ctx"]           = n_ctx;
    }
    return res;
}

//
// server_task_result_metrics
//
json server_task_result_slots::to_json() {
    return slots_data;
}

json server_task_result_metrics::to_json() {
    // not used, /metrics renders prometheus text via to_metrics()
    return json{};
}

// metrics definition: https://prometheus.io/docs/practices/naming/#metric-names
std::string server_task_result_metrics::to_metrics() {
    const std::vector<metric_item> counters = {
        {
            "prompt_tokens_total",
            "Number of prompt tokens processed, excluding cached tokens",
            (double) metrics.prompt.count
        }, {
            "prompt_tokens_cached_total",
            "Number of prompt tokens reused from the cache",
            (double) metrics.n_prompt_cached
        }, {
            "prompt_seconds_total",
            "Total time spent processing prompts",
            metrics.prompt.time / 1.e6
        }, {
            "tokens_predicted_total",
            "Number of generation tokens processed",
            (double) metrics.predict.count
        }, {
            "tokens_predicted_seconds_total",
            "Total time spent generating tokens",
            metrics.predict.time / 1.e6
        }, {
            "n_decode_total",
            "Total number of llama_decode() calls, excluding speculative decoding and multimodal decoding",
            (double) metrics.n_decode
        }, {
            "n_tokens_max",
            "Largest observed sequence length (prompt + generation)",
            (double) metrics.n_tokens_max
        }, {
            "spec_decode_num_draft_tokens_total",
            "Speculative: Total draft tokens generated",
            (double) metrics.n_draft_tokens
        }, {
            "spec_decode_num_accepted_tokens_total",
            "Speculative: Total draft tokens accepted by the target model",
            (double) metrics.n_draft_accepted
        }, {
            "spec_decode_num_drafts_total",
            "Speculative: Total speculative decoding verification steps",
            (double) metrics.n_draft_verif_steps
        },
    };

    // prompt-token decomposition: every accounted prompt token is counted
    // exactly once, either reused from one of the tiers or prefilled.
    // H_total = (slot+ram+disk)/prompt, H_disk = disk/prompt_disk_ok, where
    // prompt_disk_ok only covers the requests whose prompt reached the disk tier
    const std::vector<metric_item> cache_counters = {
        {
            "cache_request_prompt_tokens_total",
            "Prompt tokens of the requests accounted by the cache statistics",
            (double) metrics.n_cache_req_prompt
        }, {
            "cache_request_prompt_tokens_disk_ok_total",
            "Prompt tokens of the requests that queried the disk cache, hit or miss",
            (double) metrics.n_cache_req_prompt_disk_ok
        }, {
            "cache_reused_tokens_slot_total",
            "Prompt tokens reused from the KV state the slot already held",
            (double) metrics.n_cache_reused_slot
        }, {
            "cache_reused_tokens_ram_total",
            "Prompt tokens reused from the RAM prompt cache",
            (double) metrics.n_cache_reused_ram
        }, {
            "cache_reused_tokens_disk_total",
            "Prompt tokens reused from the on-disk prompt cache",
            (double) metrics.n_cache_reused_disk
        }, {
            "cache_prefill_tokens_total",
            "Prompt tokens that had to be prefilled",
            (double) metrics.n_cache_prefill
        }, {
            "cache_disk_candidate_lcp_tokens_total",
            "Sum of the best candidate prefix shared with a stored entry, accepted or not",
            (double) cache_disk.n_candidate_lcp_tokens
        }, {
            "cache_disk_lookups_total",
            "Number of prompts offered to the on-disk cache",
            (double) cache_disk.n_lookups
        }, {
            "cache_disk_bytes_read_total",
            "Bytes read from the on-disk cache for restores",
            (double) cache_disk.n_read_bytes
        }, {
            "cache_disk_bytes_written_total",
            "Bytes written into the on-disk cache",
            (double) cache_disk.n_write_bytes
        }, {
            "cache_disk_restore_seconds_total",
            "Time spent reading and restoring entries from the on-disk cache",
            cache_disk.n_restore_us / 1.e6
        }, {
            "cache_disk_manifest_fallback_total",
            "Starts that could not trust the startup index and rescanned every entry",
            (double) cache_disk.n_manifest_fallback
        }, {
            "cache_disk_enqueue_queued_total",
            "Disk cache writes accepted by the admission checks",
            (double) cache_disk.n_enqueue_queued
        }, {
            "cache_disk_enqueue_dedup_total",
            "Disk cache writes skipped because the prompt is already stored, queued or covered",
            (double) cache_disk.n_enqueue_dedup
        }, {
            "cache_disk_enqueue_rejected_total",
            "Disk cache writes rejected by the size limit or the queue budget",
            (double) cache_disk.n_enqueue_rejected
        },
    };

    const std::vector<metric_item> gauges = {
        {
            "prompt_tokens_seconds",
            "Average prompt throughput in tokens/s",
            metrics.prompt_bucket.n_per_second()
        }, {
            "predicted_tokens_seconds",
            "Average generation throughput in tokens/s",
            metrics.predict_bucket.n_per_second()
        }, {
            "requests_processing",
            "Number of requests processing",
            (double) n_processing_slots
        }, {
            "requests_deferred",
            "Number of requests deferred",
            (double) n_tasks_deferred
        }, {
            "n_busy_slots_per_decode",
            "Average number of busy slots per llama_decode() call",
            (double) metrics.n_busy_slots / std::max((double) metrics.n_decode, 1.0)
        }, {
            "cache_disk_entries",
            "Number of entries in the on-disk prompt cache",
            (double) cache_disk.n_entries
        }, {
            "cache_disk_bytes",
            "Disk usage accounted for, including entries that could not be deleted",
            (double) cache_disk.n_bytes
        }, {
            "cache_disk_quarantined",
            "Entries kept on disk after a failed deletion, excluded from lookups",
            (double) cache_disk.n_quarantined
        }, {
            "cache_disk_remove_failed_total",
            "Entry files that could not be deleted",
            (double) cache_disk.n_remove_failed
        }, {
            "cache_disk_hash_collision_total",
            "Stored paths holding a different token/media identity than the write that mapped to them",
            (double) cache_disk.n_hash_collisions
        }, {
            "cache_disk_path_conflict_total",
            "Writes refused because the content-addressed path stayed occupied",
            (double) cache_disk.n_path_conflicts
        }, {
            "cache_disk_write_rejected_total",
            "Entries rolled back because they did not fit the quota",
            (double) cache_disk.n_write_rejected
        }, {
            "cache_disk_rollback_failed_total",
            "Rejected writes whose file could not be deleted and became quarantined entries",
            (double) cache_disk.n_rollback_failed
        }, {
            "cache_disk_quota_unreclaimed_total",
            "Admission transactions that could not really reclaim the quota because a removal failed",
            (double) cache_disk.n_quota_unreclaimed
        }, {
            "cache_disk_queue_entries",
            "Number of disk cache writes waiting for the writer",
            (double) cache_disk.n_queue_entries
        }, {
            "cache_disk_queue_bytes",
            "Bytes held by the pending disk cache writes",
            (double) cache_disk.n_queue_bytes
        },
    };

    std::stringstream prometheus;

    auto add_items = [&prometheus](const char * type, const std::vector<metric_item> & items) {
        for (const auto & item : items) {
            prometheus << "# HELP llamacpp:" << item.name << " " << item.description << "\n"
                       << "# TYPE llamacpp:" << item.name << " " << type             << "\n"
                       << "llamacpp:"        << item.name << " " << item.value       << "\n";
        }
    };

    // prefix directory: discovery metadata only. none of these counters may be
    // added to the reused-token counters above - a candidate prefix is not a hit
    const std::vector<metric_item> prefix_counters = {
        {
            "cache_prefix_requests_total",
            "Requests published to the prefix directory",
            (double) cache_prefix.n_requests
        }, {
            "cache_prefix_candidates_total",
            "Common prefixes reported to callers",
            (double) cache_prefix.n_candidates
        }, {
            "cache_prefix_observed_total",
            "Prefixes that became observed (cumulative transitions)",
            (double) cache_prefix.n_observed_events
        }, {
            "cache_prefix_eligible_total",
            "Prefixes that became eligible for capture (cumulative transitions)",
            (double) cache_prefix.n_eligible_events
        }, {
            "cache_prefix_invalidated_refs_total",
            "Physical references withdrawn because the snapshot went away",
            (double) cache_prefix.n_invalidated
        }, {
            "cache_prefix_evicted_total",
            "Prefix nodes dropped by the capacity limits",
            (double) cache_prefix.n_evicted
        }, {
            "cache_prefix_rejected_total",
            "Observations refused because nothing could be evicted",
            (double) cache_prefix.n_rejected
        }, {
            "cache_prefix_materialize_scheduled_total",
            "Prefix captures planned; not yet a file on disk",
            (double) n_materialize_scheduled
        }, {
            "cache_prefix_materialize_exported_total",
            "Prefix captures whose complete state was exported",
            (double) n_materialize_exported
        }, {
            "cache_prefix_materialize_skipped_total",
            "Prefix captures that produced no state (refused, past the boundary, export failed)",
            (double) n_materialize_skipped
        },
    };

    const std::vector<metric_item> prefix_gauges = {
        {
            "cache_prefix_nodes",
            "Prefix nodes currently held",
            (double) cache_prefix.n_nodes
        }, {
            "cache_prefix_tokens",
            "Token/media units currently indexed",
            (double) cache_prefix.n_tokens
        }, {
            "cache_prefix_bytes",
            "Approximate memory held by the prefix directory",
            (double) cache_prefix.bytes
        }, {
            "cache_prefix_persisted_refs",
            "Prefixes currently backed by a validated snapshot",
            (double) cache_prefix.n_persisted
        }, {
            "cache_prefix_snapshots",
            "Live physical references",
            (double) cache_prefix.n_snapshots
        }, {
            "cache_prefix_unavailable",
            "Prefixes whose snapshot was withdrawn",
            (double) cache_prefix.n_unavailable
        },
    };

    add_items("counter", counters);
    add_items("counter", cache_counters);
    add_items("counter", prefix_counters);
    add_items("gauge",   gauges);
    add_items("gauge",   prefix_gauges);

    // labeled counter: one series per reject reason
    prometheus << "# HELP llamacpp:cache_disk_reject_total Prompts the on-disk cache could not serve, by reason" << "\n"
               << "# TYPE llamacpp:cache_disk_reject_total counter" << "\n";
    for (size_t i = (size_t) cache_reject_reason::none + 1; i < (size_t) cache_reject_reason::count; i++) {
        prometheus << "llamacpp:cache_disk_reject_total{reason=\"" << cache_reject_reason_name((cache_reject_reason) i) << "\"} "
                   << cache_disk.n_reject[i] << "\n";
    }

    return prometheus.str();
}

//
// server_task_result_slot_save_load
//
json server_task_result_slot_save_load::to_json() {
    if (is_save) {
        return json {
            { "id_slot",   id_slot },
            { "filename",  filename },
            { "n_saved",   n_tokens },
            { "n_written", n_bytes },
            { "timings", {
                { "save_ms", t_ms }
            }},
        };
    }

    return json {
        { "id_slot",    id_slot },
        { "filename",   filename },
        { "n_restored", n_tokens },
        { "n_read",     n_bytes },
        { "timings", {
            { "restore_ms", t_ms }
        }},
    };
}

//
// server_task_result_slot_erase
//
json server_task_result_slot_erase::to_json() {
    return json {
        { "id_slot",  id_slot },
        { "n_erased", n_erased },
    };
}

//
// server_task_result_get_lora
//

json server_task_result_get_lora::to_json() {
    json result = json::array();
    for (size_t i = 0; i < loras.size(); ++i) {
        auto & lora = loras[i];
        json entry = {
            {"id",            i},
            {"path",          lora.info.path},
            {"scale",         lora.info.scale},
            {"task_name",     lora.info.task_name},
            {"prompt_prefix", lora.info.prompt_prefix},
        };
        if (!lora.alora_invocation_tokens.empty()) {
            entry["alora_invocation_string"] = lora.alora_invocation_string;
            entry["alora_invocation_tokens"] = lora.alora_invocation_tokens;
        }
        result.push_back(std::move(entry));
    }
    return result;
}

//
// server_task_result_apply_lora
//

json server_task_result_apply_lora::to_json() {
    return json {{ "success", true }};
}

//
// server_prompt_cache
//
//
// server_disk_cache
//
// second-level prompt cache. Entries are written in the background as soon as
// they are evicted from the RAM cache, and restored on a prefix match - before
// the prompt would be prefilled again.
//
// entry file layout (little-endian):
//   char     magic[8]      "LLDSC001"
//   uint32   version
//   uint32   sig_len
//   char     sig[sig_len]  model/context signature
//   uint32   packed_size
//   char     packed[packed_size]  token ids plus media metadata
//   uint64   size_main
//   uint64   size_drft
//   uint32   n_ckpt
//   uint64   created       unix seconds
//   uint64   last_used     unix seconds (rewritten in place on every hit)
//   uint64   crc_main      CRC64 of the main state payload
//   uint64   crc_drft      CRC64 of the draft state payload
//   uint64   crc_packed    CRC64 of packed[packed_size] (version 8)
//   checkpoint table: n_ckpt x { int64 n_tokens, int32 pos_min, int32 pos_max,
//                                uint64 size_spec, uint64 size_tgt, uint64 size_dft,
//                                uint64 crc_spec, uint64 crc_tgt, uint64 crc_dft }
//   main[size_main]
//   drft[size_drft]
//   checkpoint blobs, in table order: spec, tgt, dft
//

const char * disk_cache_remove_status_name(disk_cache_remove_status status) {
    switch (status) {
        case disk_cache_remove_status::removed:         return "removed";
        case disk_cache_remove_status::already_missing: return "already_missing";
        case disk_cache_remove_status::failed:          return "failed";
    }

    return "unknown";
}

const char * disk_cache_path_owner_name(disk_cache_path_owner owner) {
    switch (owner) {
        case disk_cache_path_owner::free:      return "free";
        case disk_cache_path_owner::identical: return "identical";
        case disk_cache_path_owner::conflict:  return "conflict";
    }

    return "unknown";
}

const char * cache_reject_reason_name(cache_reject_reason reason) {
    switch (reason) {
        case cache_reject_reason::none:           return "none";
        case cache_reject_reason::no_prefix:      return "no_prefix";
        case cache_reject_reason::too_short:      return "too_short";
        case cache_reject_reason::media_mismatch: return "media_mismatch";
        case cache_reject_reason::io_cost:        return "io_cost";
        case cache_reject_reason::checksum_error: return "checksum_error";
        case cache_reject_reason::read_error:     return "read_error";
        case cache_reject_reason::restore_failed: return "restore_failed";
        case cache_reject_reason::signature_mismatch: return "signature_mismatch";
        case cache_reject_reason::count:          break;
    }

    return "unknown";
}

namespace {

constexpr char     DISK_CACHE_MAGIC[8] = { 'L', 'L', 'D', 'S', 'C', '0', '0', '1' };
constexpr uint32_t DISK_CACHE_VERSION  = 8;

// version 5 added the last_used field, version 6 added the crc_main/crc_drft
// integrity checksums, version 7 added per-checkpoint checksums, version 8 adds
// crc_packed - the writer-side checksum of the token/media metadata, which is the
// only check that covers corruption that happened before the entry was indexed
//
// Header layout is fixed except for the model/context signature and the packed
// metadata, so its size is DISK_CACHE_HEADER_FIXED + sig.size() + packed_size
// bytes. touch_entry() rewrites the last_used field in place, so its offset must
// match this layout exactly - it is derived from the same constants here instead
// of being written out as a literal
constexpr size_t DISK_CACHE_HDR_PREFIX        = 8 + 4 + 4 + 4; // magic, version, sig_len, packed_size
constexpr size_t DISK_CACHE_HDR_BEFORE_LAST   = 8 + 8 + 4 + 8; // size_main, size_drft, n_ckpt, created
constexpr size_t DISK_CACHE_HDR_AFTER_LAST    = 8 + 8 + 8;     // crc_main, crc_drft, crc_packed
constexpr size_t DISK_CACHE_HEADER_FIXED      = DISK_CACHE_HDR_PREFIX + DISK_CACHE_HDR_BEFORE_LAST + 8 + DISK_CACHE_HDR_AFTER_LAST;

// offset of the last_used field relative to the start of the file, excluding the
// variable-length signature and packed metadata
constexpr size_t DISK_CACHE_HDR_LAST_USED_OFFSET = DISK_CACHE_HDR_PREFIX + DISK_CACHE_HDR_BEFORE_LAST;

static_assert(DISK_CACHE_HEADER_FIXED == 80, "the fixed header size must match the layout documented above");
static_assert(DISK_CACHE_HDR_LAST_USED_OFFSET == 48, "touch_entry depends on this offset");

// skip entries shorter than this in the lookup: the lcp with any prompt
// cannot reach min_tokens, so they can never be restored
constexpr size_t DISK_CACHE_MIN_LOOKUP_TOKENS = 64;

// cap the number of pending writes: the queued entries share the RAM cache
// buffers (shared_ptr, no extra memory), but a slow disk must not pin an
// unbounded number of them. the byte budget itself is configurable through
// --cache-disk-queue-max (common_params::cache_disk_queue_max_mib)
constexpr size_t DISK_CACHE_QUEUE_MAX_ENTRIES = 8;

// the cost guard itself (DISK_CACHE_MAX_BYTES_PER_TOKEN) lives in
// server-disk-cache-match.h, so that load() and every write-suppression path
// share one definition

// replacing a stored entry rewrites the whole file, and an entry carries a
// fixed-size state next to the KV payload: a 120k-token entry is ~2.7 GiB, i.e.
// ~18 s on a 150 MiB/s cache disk, while prefilling a 4k-token tail at ~1.3k
// tok/s costs ~3 s. so when an existing entry is a prefix of the new prompt and
// only a short tail is missing, storing the longer one costs more than it saves.
// the stored prefix is allowed to lag the live prompt by at most this many
// tokens before the rewrite is accepted, which caps the extra prefill a restore
// has to pay and still removes most of the rewrite traffic during a conversation
constexpr size_t DISK_CACHE_MAX_PREFIX_LAG_TOKENS = 4096;

// startup index file: lets scan() trust entry metadata without opening and
// parsing every entry file. rewritten on every mutation, ignored when the
// signature, version or any file size does not match.
// layout: magic, version, sig_len, sig, body_size, body[...], body_crc64 - the
// whole record block is checksummed, so a torn or bit-flipped index falls back
// to a full scan instead of silently feeding wrong token prefixes to the lookup
constexpr char     DISK_CACHE_INDEX_MAGIC[8] = { 'L', 'L', 'D', 'S', 'C', 'I', 'D', 'X' };
constexpr uint32_t DISK_CACHE_INDEX_VERSION  = 3; // v2 added size_main/size_drft, v3 added packed_crc + body checksum
constexpr const char * DISK_CACHE_INDEX_NAME = "pc-index.dat";

// how many entries a single lookup may try before giving up and prefilling.
// a rejected entry (corrupt file, unreadable, missing draft state) must not
// force a full prefill while a shorter entry can still cover part of the prompt
constexpr int DISK_CACHE_MAX_LOAD_ATTEMPTS = 4;

// background writer yields the CPU for this long after every chunk written,
// so a burst of large evictions does not starve inference of disk IO
constexpr size_t DISK_CACHE_WRITE_THROTTLE_CHUNK = 256ull*1024ull*1024ull;
constexpr int64_t DISK_CACHE_WRITE_THROTTLE_US   = 10000; // 10 ms per 256 MiB

// CRC64-ECMA (reflected polynomial 0xC96C5795D7870F42), slice-by-8. Detects
// torn writes and on-disk corruption of the KV state payloads before they are
// handed to llama_state_seq_set_data_ext, which would otherwise accept garbage.
// The 8-way table unrolls the byte-at-a-time dependency chain, which matters on
// the multi-gigabyte payloads this cache reads and writes; the resulting
// checksum is identical to the classic single-table form.
uint64_t disk_cache_crc64(const uint8_t * data, size_t size) {
    static const std::array<std::array<uint64_t, 256>, 8> tables = []() {
        std::array<std::array<uint64_t, 256>, 8> t{};
        for (uint64_t i = 0; i < 256; ++i) {
            uint64_t crc = i;
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xC96C5795D7870F42ull & (~(crc & 1) + 1));
            }
            t[0][(size_t) i] = crc;
        }
        for (int n = 1; n < 8; ++n) {
            for (int k = 0; k < 256; ++k) {
                const uint64_t c = t[n - 1][k];
                t[n][k] = (c >> 8) ^ t[0][c & 0xFF];
            }
        }
        return t;
    }();

    const auto & t0 = tables[0];

    uint64_t crc = 0xFFFFFFFFFFFFFFFFull;

    size_t i = 0;

    for (; i + 8 <= size; i += 8) {
        // little-endian 8-byte word, assembled byte-wise to stay endian-independent
        const uint64_t d = (uint64_t) data[i    ]        | (uint64_t) data[i + 1] <<  8 |
                           (uint64_t) data[i + 2] << 16 | (uint64_t) data[i + 3] << 24 |
                           (uint64_t) data[i + 4] << 32 | (uint64_t) data[i + 5] << 40 |
                           (uint64_t) data[i + 6] << 48 | (uint64_t) data[i + 7] << 56;

        crc ^= d;

        crc = tables[7][ crc         & 0xFF] ^ tables[6][(crc >>  8) & 0xFF] ^
              tables[5][(crc >> 16) & 0xFF] ^ tables[4][(crc >> 24) & 0xFF] ^
              tables[3][(crc >> 32) & 0xFF] ^ tables[2][(crc >> 40) & 0xFF] ^
              tables[1][(crc >> 48) & 0xFF] ^ t0      [ crc >> 56        ];
    }

    for (; i < size; ++i) {
        crc = t0[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFFFFFFFFFFull;
}

// any contiguous byte container (common_state_data, std::vector<uint8_t>, ...)
template <typename T, typename std::enable_if<std::is_same<typename T::value_type, uint8_t>::value, int>::type = 0>
uint64_t disk_cache_crc64(const T & data) {
    return data.empty() ? 0 : disk_cache_crc64(data.data(), data.size());
}

// the packed token/media metadata is kept as raw chars
inline uint64_t disk_cache_crc64(const std::vector<char> & data) {
    return data.empty() ? 0 : disk_cache_crc64(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

uint64_t disk_cache_hash(const llama_tokens & tokens, const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids) {
    uint64_t h = 1469598103934665603ull; // FNV-1a
    auto add = [&](uint8_t value) {
        h ^= value;
        h *= 1099511628211ull;
    };
    auto add_u32 = [&](uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            add((uint8_t) ((value >> (8*i)) & 0xFF));
        }
    };
    for (const llama_token t : tokens) {
        add_u32((uint32_t) t);
    }
    add_u32((uint32_t) media_indices.size());
    for (size_t i = 0; i < media_indices.size(); ++i) {
        add_u32(media_indices[i]);
        add_u32(media_token_counts[i]);
        add_u32((uint32_t) media_ids[i].size());
        for (unsigned char c : media_ids[i]) {
            add(c);
        }
    }
    return h;
}

int64_t disk_cache_now() {
    return (int64_t) std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

bool disk_cache_unpack_ids(const std::vector<char> & packed, llama_tokens & ids,
        std::vector<uint32_t> & media_indices, std::vector<uint32_t> & media_token_counts,
        std::vector<std::string> & media_ids) {
    if (packed.size() < sizeof(uint32_t) * 2) {
        return false;
    }

    size_t offset = 0;
    auto read = [&](void * dst, size_t size) {
        if (offset > packed.size() || size > packed.size() - offset) {
            return false;
        }
        memcpy(dst, packed.data() + offset, size);
        offset += size;
        return true;
    };
    auto read_string = [&](std::string & value) {
        uint32_t size = 0;
        if (!read(&size, sizeof(size)) || size > packed.size() - offset) {
            return false;
        }
        value.assign(packed.data() + offset, size);
        offset += size;
        return true;
    };

    uint32_t n_tokens = 0;
    if (!read(&n_tokens, sizeof(n_tokens)) || n_tokens > (packed.size() - offset) / sizeof(llama_token)) {
        return false;
    }
    ids.resize(n_tokens);
    if (n_tokens > 0 && !read(ids.data(), (size_t) n_tokens * sizeof(llama_token))) {
        return false;
    }

    uint32_t n_media = 0;
    if (!read(&n_media, sizeof(n_media)) || n_media > (packed.size() - offset) / sizeof(uint32_t)) {
        return false;
    }
    media_indices.resize(n_media);
    media_token_counts.resize(n_media);
    media_ids.resize(n_media);
    size_t previous_end = 0;
    for (uint32_t i = 0; i < n_media; ++i) {
        if (!read(&media_indices[i], sizeof(uint32_t)) || !read(&media_token_counts[i], sizeof(uint32_t)) ||
                !read_string(media_ids[i])) {
            return false;
        }
        if (media_indices[i] < previous_end || media_indices[i] >= ids.size() || media_token_counts[i] == 0 ||
                media_token_counts[i] > ids.size() - media_indices[i] || media_ids[i].empty()) {
            return false;
        }
        previous_end = (size_t) media_indices[i] + media_token_counts[i];
        for (size_t j = media_indices[i]; j < previous_end; ++j) {
            if (ids[j] != LLAMA_TOKEN_NULL) {
                return false;
            }
        }
    }
    return offset == packed.size();
}

bool disk_cache_pack_ids(const llama_tokens & ids, const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids,
        std::vector<char> & packed);

bool disk_cache_pack(const server_tokens & tokens, std::vector<char> & packed, llama_tokens & ids,
        std::vector<uint32_t> & media_indices, std::vector<uint32_t> & media_token_counts,
        std::vector<std::string> & media_ids) {
    if (tokens.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    ids.resize(tokens.size());
    for (size_t i = 0; i < tokens.size(); ++i) {
        ids[i] = tokens[i];
    }

    try {
        media_indices = tokens.media_chunk_indices();
        media_token_counts = tokens.media_chunk_token_counts();
        media_ids.clear();
        media_ids.reserve(media_indices.size());
        for (uint32_t index : media_indices) {
            const std::string id = tokens.media_chunk_id(index);
            if (id.empty()) {
                return false;
            }
            media_ids.push_back(id);
        }
    } catch (const std::exception &) {
        return false;
    }

    return disk_cache_pack_ids(ids, media_indices, media_token_counts, media_ids, packed);
}

bool disk_cache_pack_ids(const llama_tokens & ids, const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids,
        std::vector<char> & packed) {
    packed.clear();
    if (ids.size() > std::numeric_limits<uint32_t>::max() ||
            media_indices.size() != media_token_counts.size() || media_indices.size() != media_ids.size() ||
            media_indices.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    auto append = [&](const void * data, size_t size) {
        const auto * first = reinterpret_cast<const char *>(data);
        packed.insert(packed.end(), first, first + size);
    };

    auto append_string = [&](const std::string & value) {
        if (value.size() > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        const uint32_t size = (uint32_t) value.size();
        append(&size, sizeof(size));
        if (size > 0) {
            append(value.data(), size);
        }
        return true;
    };

    const uint32_t n_tokens = (uint32_t) ids.size();
    append(&n_tokens, sizeof(n_tokens));
    if (!ids.empty()) {
        append(ids.data(), ids.size() * sizeof(llama_token));
    }
    const uint32_t n_media = (uint32_t) media_indices.size();
    append(&n_media, sizeof(n_media));
    for (size_t i = 0; i < media_indices.size(); ++i) {
        append(&media_indices[i], sizeof(media_indices[i]));
        append(&media_token_counts[i], sizeof(media_token_counts[i]));
        if (!append_string(media_ids[i])) {
            return false;
        }
    }
    return true;
}

template <typename T>
void disk_cache_write(std::ostream & f, const T & v) {
    f.write(reinterpret_cast<const char *>(&v), sizeof(T));
}

template <typename T>
bool disk_cache_read(std::istream & f, T & v) {
    f.read(reinterpret_cast<char *>(&v), sizeof(T));
    return (bool) f;
}

} // namespace

server_disk_cache::server_disk_cache(const std::string & dir, const std::string & sig, int32_t limit_size_mib, int32_t min_tokens, int32_t ttl_seconds, int32_t queue_max_mib)
    : dir(dir), sig(sig) {
    namespace fs = std::filesystem;

    limit_size = limit_size_mib < 0 ? 0 : (size_t) limit_size_mib * 1024ull * 1024ull;
    this->min_tokens = (size_t) std::max(0, min_tokens);
    this->ttl_seconds = std::max(0, ttl_seconds);
    queue_bytes_max = queue_max_mib < 0 ? 0 : (size_t) queue_max_mib * 1024ull * 1024ull;

    if (limit_size_mib == 0) {
        SRV_INF("%s", "disk cache: disabled by --cache-disk-max 0\n");
        active = false;
        return;
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        SRV_ERR("disk cache: failed to create directory '%s': %s\n", dir.c_str(), ec.message().c_str());
        active = false;
        return;
    }

    scan();

    {
        std::lock_guard<std::mutex> lock(mtx);

        const size_t n_after_scan = entries.size();

        // entries stored before prefix dedup was added may still hold a chain
        // of prefix prompts - drop the redundant ones
        dedup_prefixes();

        // trim an existing cache that no longer fits the current limit (e.g. after lowering --cache-disk-max)
        enforce_limit();

        // persist the startup index so the next boot can trust the metadata
        // without opening every entry file
        save_manifest();

        // the lookup only becomes reachable after this block: entries, the dedup
        // pass and the quota trim are all settled before the first request can
        // call load(), so a startup race cannot make the first request miss
        SRV_INF("disk cache: cache_index_ready: %zu entries after scan, %zu after dedup/trim, %.2f MiB\n",
                n_after_scan, entries.size(), total_size/(1024.0*1024.0));
    }

    th = std::thread([this]() { worker(); });

    SRV_INF("disk cache: dir = '%s', %zu entries, %.2f MiB used, limit %.2f MiB, min %zu tokens%s\n",
            dir.c_str(), entries.size(), total_size/(1024.0*1024.0), limit_size/(1024.0*1024.0), this->min_tokens,
            this->ttl_seconds > 0 ? string_format(", ttl %lld s", (long long) this->ttl_seconds).c_str() : "");
}

server_disk_cache::~server_disk_cache() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        stop_flag = true;
    }

    cv.notify_all();

    if (th.joinable()) {
        th.join(); // the worker drains the queue before exiting
    }

    // persist the in-memory last_used timestamps (hits since the scan) into
    // the manifest first: on the next boot scan() trusts the manifest, so
    // writing only the per-entry header here would be overwritten and the
    // cross-restart LRU order would silently reset
    {
        std::lock_guard<std::mutex> lock(mtx);
        save_manifest();
    }

    // then still update the per-entry headers (best-effort, used when the
    // manifest is missing or rejected); a failure only degrades back to
    // creation-time order
    for (const auto & e : entries) {
        if (e.last_used_dirty) {
            touch_entry(e);
        }
    }
}

size_t server_disk_cache::n_entries() const {
    std::lock_guard<std::mutex> lock(mtx);
    return entries.size();
}

size_t server_disk_cache::size() const {
    std::lock_guard<std::mutex> lock(mtx);
    return total_size;
}

disk_cache_prompt_identity server_disk_cache::identity_of(const server_disk_cache_entry & e) {
    disk_cache_prompt_identity id;
    id.tokens             = &e.tokens;
    id.media_indices      = &e.media_indices;
    id.media_token_counts = &e.media_token_counts;
    id.media_ids          = &e.media_ids;
    id.file_bytes         = e.size_file;
    return id;
}

bool server_disk_cache::is_reusable_entry(const server_disk_cache_entry & e) {
    // a quarantined entry still occupies disk space but may not serve a lookup or
    // stand in for a write; an entry without tokens has nothing to compare
    return !e.quarantined && !e.tokens.empty();
}

disk_cache_path_owner server_disk_cache::claim_path_locked(uint64_t hash, const std::string & path,
        const disk_cache_prompt_identity & wanted, const char * what) {
    // the file name is derived from the content hash, so every entry that maps to
    // this hash shares the path - possibly with a different identity, and possibly
    // quarantined. the path has to be resolved before anything is written: the
    // writer never overwrites a file whose owner it has not identified
    bool occupied = false;

    for (auto it = entries.begin(); it != entries.end(); ) {
        if (it->path != path) {
            ++it;
            continue;
        }

        occupied = true;

        if (!is_reusable_entry(*it)) {
            // a quarantined entry still owns its bytes, but it may not keep a path
            // that a valid write needs. reclaim it for real: only a confirmed
            // removal frees the path and adjusts the ledger
            if (remove_file(path) == disk_cache_remove_status::failed) {
                SRV_ERR("disk cache: cannot %s to '%s': the path is held by a quarantined entry that could not be removed\n",
                        what, path.c_str());
                counters.n_path_conflicts++;
                return disk_cache_path_owner::conflict;
            }

            SRV_INF("disk cache: reclaimed '%s' from a quarantined entry (%.2f MiB)\n",
                    path.c_str(), it->size_file/(1024.0*1024.0));

            total_size -= std::min(total_size, it->size_file);
            notify_invalidated(path);
            it = entries.erase(it);
            save_manifest();
            continue;
        }

        if (it->hash == hash &&
                disk_cache_classify_duplicate(identity_of(*it), wanted, true) == disk_cache_duplicate::identical) {
            return disk_cache_path_owner::identical;
        }

        // a usable entry with a different identity holds the same name: refusing is
        // the only safe answer while the file name is the hash alone
        SRV_ERR("disk cache: '%s' is held by a different prompt (%zu tokens vs %zu), refusing the %s\n",
                path.c_str(), it->tokens.size(),
                wanted.tokens == nullptr ? 0 : wanted.tokens->size(), what);
        counters.n_hash_collisions++;
        counters.n_path_conflicts++;
        return disk_cache_path_owner::conflict;
    }

    if (occupied) {
        // every record at this path was reclaimed above
        return disk_cache_path_owner::free;
    }

    return disk_cache_path_owner::free;
}

disk_cache_remove_status server_disk_cache::remove_file(const std::string & path) const {
    // the one place that deletes entry files. only a confirmed removal (or a file
    // that is already gone) may be followed by reclaiming quota - treating a
    // sharing violation as success silently under-reports disk usage, which then
    // lets enforce_limit over-fill the directory
    std::error_code ec;
    std::filesystem::remove(path, ec);

    if (!ec) {
        return disk_cache_remove_status::removed;
    }

    if (ec == std::errc::no_such_file_or_directory) {
        return disk_cache_remove_status::already_missing;
    }

    // the error code is the fact; the counter is not part of the const entry set
    const_cast<server_disk_cache *>(this)->counters.n_remove_failed++;

    SRV_WRN("disk cache: failed to remove '%s': %s\n", path.c_str(), ec.message().c_str());

    return disk_cache_remove_status::failed;
}

void server_disk_cache::quarantine_entry(server_disk_cache_entry & e, const char * why) {
    if (e.quarantined) {
        return;
    }

    e.quarantined = true;

    SRV_WRN("disk cache: '%s' %s, keeping %.2f MiB accounted for and excluding it from lookups\n",
            e.path.c_str(), why, e.size_file/(1024.0*1024.0));
}

void server_disk_cache::notify_committed(const server_disk_cache_entry & e) {
    if (!on_entry_committed) {
        return;
    }

    on_entry_committed(e, ++event_epoch);
}

void server_disk_cache::notify_invalidated(const std::string & path) {
    if (!on_entry_invalidated) {
        return;
    }

    on_entry_invalidated(path, ++event_epoch);
}

std::vector<server_disk_cache_entry> server_disk_cache::entries_snapshot() const {
    std::lock_guard<std::mutex> lock(mtx);
    return entries;
}

disk_cache_counters server_disk_cache::stats() const {
    std::lock_guard<std::mutex> lock(mtx);

    // `counters` is written from the lookup path (main thread) and from the
    // writer thread; the copy is only a metrics snapshot, and the sizes of
    // `entries`/`queue` must be read under the lock anyway
    disk_cache_counters out = counters;

    out.n_entries       = entries.size();
    out.n_bytes         = total_size;
    out.n_queue_entries = queue.size();

    for (const auto & e : entries) {
        out.n_quarantined += e.quarantined ? 1 : 0;
    }
    out.n_queue_bytes   = queue_bytes;

    return out;
}

void server_disk_cache::scan() {
    namespace fs = std::filesystem;

    std::error_code ec;

    // counts reported in the startup summary: without them a "cache looks empty
    // after a restart" report cannot be told apart from a corrupt index, a
    // format change or an unreadable directory
    size_t n_from_manifest = 0;
    size_t n_from_header   = 0;
    size_t n_manifest_stale = 0; // record present but the header had to be parsed anyway
    size_t n_dropped       = 0;
    size_t n_removed_tmp   = 0;

    const int64_t t_scan_start = ggml_time_us();

    SRV_INF("disk cache: cache_scan_begin: dir = '%s', format v%u\n", dir.c_str(), (unsigned) DISK_CACHE_VERSION);

    // startup index written by a previous run: when a file's size matches its
    // record, the metadata is trusted and the entry file is not opened at all.
    // anything inconsistent (unknown file, size mismatch, corrupt record) falls
    // back to the full header validation below.
    std::map<std::string, disk_cache_manifest_meta> manifest;
    const bool manifest_ok = load_manifest(manifest);

    if (!manifest_ok) {
        counters.n_manifest_fallback++;
    }

    SRV_INF("disk cache: cache_scan_manifest: %s, %zu record(s)\n",
            manifest_ok ? "hit" : "miss (full header scan)", manifest.size());

    for (const auto & de : fs::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }

        if (!de.is_regular_file(ec)) {
            continue;
        }

        const std::string name = de.path().filename().string();

        // clean up orphaned temp files left behind when the process is killed
        // between writing and renaming (entry or index files): they are never
        // picked up below and would otherwise accumulate on disk
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            std::error_code ec_rm;
            fs::remove(de.path(), ec_rm);
            if (!ec_rm) {
                n_removed_tmp++;
                SRV_WRN("disk cache: removed orphaned temp file '%s'\n", de.path().string().c_str());
            }
            continue;
        }

        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".bin") != 0) {
            continue;
        }

        std::error_code ec_size;
        const uintmax_t fsize = fs::file_size(de.path(), ec_size);
        if (ec_size) {
            // vanished or unreadable mid-scan - not worth keeping either way
            std::error_code ec_rm;
            fs::remove(de.path(), ec_rm);
            continue;
        }

        if (manifest_ok) {
            const auto mi = manifest.find(name);
            if (mi != manifest.end() && mi->second.size_file == (uint64_t) fsize) {
                server_disk_cache_entry e;
                e.path       = de.path().string();
                e.hash       = mi->second.hash;
                e.size_file  = (size_t) fsize;
                e.size_main  = (size_t) mi->second.size_main;
                e.size_drft  = (size_t) mi->second.size_drft;
                e.last_used  = mi->second.last_used;
                e.packed_crc = mi->second.packed_crc;

                // a corrupt index record is not trusted: fall through to the
                // full validation, which either repairs or drops the entry
                if (disk_cache_unpack_ids(mi->second.packed, e.tokens, e.media_indices, e.media_token_counts, e.media_ids) &&
                        e.hash == disk_cache_hash(e.tokens, e.media_indices, e.media_token_counts, e.media_ids)) {
                    total_size += e.size_file;
                    entries.push_back(std::move(e));
                    manifest.erase(mi);
                    n_from_manifest++;
                    continue;
                }

                // the record claims this file but its metadata does not decode
                // consistently - count it, the header below is the authority
                n_manifest_stale++;
            }
        }

        std::ifstream f(de.path(), std::ios::binary);
        if (!f) {
            continue;
        }

        auto drop = [&](const char * reason) {
            SRV_WRN("disk cache: dropping '%s': %s\n", de.path().string().c_str(), reason);
            f.close();

            std::error_code ec_rm;
            fs::remove(de.path(), ec_rm);
            n_dropped++;
        };

        char magic[8];
        f.read(magic, sizeof(magic));

        uint32_t version = 0;
        uint32_t sig_len = 0;
        if (!disk_cache_read(f, version) || !disk_cache_read(f, sig_len) ||
                memcmp(magic, DISK_CACHE_MAGIC, sizeof(magic)) != 0) {
            drop("bad header");
            continue;
        }

        if (version != DISK_CACHE_VERSION) {
            // never reinterpret an older layout as the current one: v7 files have
            // no packed checksum, so they cannot be validated on read. the cache
            // is a pure accelerator, dropping and rebuilding it is safe - but the
            // discard must be stated instead of looking like a corruption
            SRV_WRN("disk cache: dropping '%s': entry format v%u %s v%u, the cache will be rebuilt\n",
                    de.path().string().c_str(), (unsigned) version,
                    version < DISK_CACHE_VERSION ? "is older than" : "is newer than", (unsigned) DISK_CACHE_VERSION);
            f.close();
            std::error_code ec_rm;
            fs::remove(de.path(), ec_rm);
            n_dropped++;
            continue;
        }

        if (sig_len != sig.size()) {
            drop("different model or context configuration");
            continue;
        }

        std::string entry_sig(sig_len, '\0');
        f.read(entry_sig.data(), (std::streamsize) sig_len);
        if (entry_sig != sig) {
            drop("different model or context configuration");
            continue;
        }

        uint32_t packed_size = 0;
        if (!disk_cache_read(f, packed_size) || packed_size > (1u << 28)) {
            drop("bad prompt metadata size");
            continue;
        }

        std::vector<char> packed(packed_size);
        if (packed_size > 0) {
            f.read(packed.data(), (std::streamsize) packed.size());
        }

        server_disk_cache_entry e;
        e.path = de.path().string();
        if (!f || !disk_cache_unpack_ids(packed, e.tokens, e.media_indices, e.media_token_counts, e.media_ids)) {
            drop("truncated tokens");
            continue;
        }

        uint64_t size_main   = 0;
        uint64_t size_drft   = 0;
        uint32_t n_ckpt      = 0;
        uint64_t created     = 0;
        uint64_t last_used   = 0;
        uint64_t crc_main    = 0;
        uint64_t crc_drft    = 0;
        uint64_t crc_packed  = 0;
        if (!disk_cache_read(f, size_main) || !disk_cache_read(f, size_drft) ||
                !disk_cache_read(f, n_ckpt) || !disk_cache_read(f, created) ||
                !disk_cache_read(f, last_used) || !disk_cache_read(f, crc_main) ||
                !disk_cache_read(f, crc_drft) || !disk_cache_read(f, crc_packed)) {
            drop("truncated header");
            continue;
        }

        // the packed metadata is verified here as well: scan is the last point
        // where a corrupt header can be dropped without a lookup ever seeing it
        (void) crc_main;
        (void) crc_drft;
        if (disk_cache_crc64(packed) != crc_packed) {
            drop("prompt metadata checksum mismatch");
            continue;
        }

        e.hash       = disk_cache_hash(e.tokens, e.media_indices, e.media_token_counts, e.media_ids);
        e.size_main  = (size_t) size_main;
        e.size_drft  = (size_t) size_drft;
        e.last_used  = (int64_t) (last_used > 0 ? last_used : created);
        e.size_file  = (size_t) fsize; // stat'ed above, before the header was parsed
        e.packed_crc = crc_packed;

        total_size += e.size_file;
        entries.push_back(std::move(e));
        n_from_header++;
    }

    SRV_INF("disk cache: cache_scan_end: %zu entries (manifest %zu, header %zu, stale index records %zu), %zu dropped, %zu temp removed, %.2f MiB, %.2f ms\n",
            entries.size(), n_from_manifest, n_from_header, n_manifest_stale, n_dropped, n_removed_tmp,
            total_size/(1024.0*1024.0), (ggml_time_us() - t_scan_start)/1000.0);
}

bool server_disk_cache::load_manifest(std::map<std::string, disk_cache_manifest_meta> & out) {
    std::ifstream f(dir + "/" + DISK_CACHE_INDEX_NAME, std::ios::binary);
    if (!f) {
        return false; // no index from a previous run - a full scan is the normal path
    }

    char magic[8];
    f.read(magic, sizeof(magic));

    uint32_t version = 0;
    uint32_t sig_len = 0;
    if (!f || memcmp(magic, DISK_CACHE_INDEX_MAGIC, sizeof(magic)) != 0 ||
            !disk_cache_read(f, version) || version != DISK_CACHE_INDEX_VERSION ||
            !disk_cache_read(f, sig_len) || sig_len != sig.size()) {
        SRV_INF("disk cache: index rejected (header/v%u/sig mismatch), falling back to a full scan\n",
                (unsigned) version);
        return false;
    }

    std::string entry_sig(sig_len, '\0');
    f.read(entry_sig.data(), (std::streamsize) sig_len);
    if (!f || entry_sig != sig) {
        SRV_INF("%s", "disk cache: index rejected (different model or context configuration), falling back to a full scan\n");
        return false;
    }

    uint64_t body_size = 0;
    if (!disk_cache_read(f, body_size) || body_size > (1ull << 32)) {
        SRV_INF("%s", "disk cache: index rejected (bad body size), falling back to a full scan\n");
        return false;
    }

    std::vector<char> body((size_t) body_size);
    if (body_size > 0) {
        f.read(body.data(), (std::streamsize) body.size());
    }

    uint64_t body_crc = 0;
    if (!f || !disk_cache_read(f, body_crc)) {
        SRV_INF("%s", "disk cache: index rejected (truncated), falling back to a full scan\n");
        return false;
    }

    // the whole record block is checksummed: a torn write or a bit flip in the
    // index must not feed a wrong token prefix into the lookup. entries are only
    // an accelerator, so the fallback is a full header scan, not an error
    if (disk_cache_crc64(body) != body_crc) {
        SRV_WRN("%s", "disk cache: index checksum mismatch, falling back to a full scan\n");
        return false;
    }

    // parse the verified body through an in-memory stream
    std::string body_str(body.begin(), body.end());
    std::istringstream bf(body_str, std::ios::binary);

    uint32_t n_records = 0;
    if (!disk_cache_read(bf, n_records) || n_records > (1u << 20)) {
        return false;
    }

    for (uint32_t i = 0; i < n_records; ++i) {
        uint32_t name_len = 0;
        if (!disk_cache_read(bf, name_len) || name_len == 0 || name_len > 256) {
            return false;
        }

        disk_cache_manifest_meta m;

        std::string name(name_len, '\0');
        bf.read(name.data(), (std::streamsize) name_len);
        uint64_t packed_size = 0;
        if (!bf ||
                !disk_cache_read(bf, m.hash) || !disk_cache_read(bf, m.size_file) ||
                !disk_cache_read(bf, m.size_main) || !disk_cache_read(bf, m.size_drft) ||
                !disk_cache_read(bf, m.last_used) || !disk_cache_read(bf, m.packed_crc) ||
                !disk_cache_read(bf, packed_size) || packed_size > (1u << 28)) {
            return false;
        }

        m.packed.resize((size_t) packed_size);
        if (packed_size > 0) {
            bf.read(m.packed.data(), (std::streamsize) m.packed.size());
        }
        if (!bf) {
            return false;
        }

        out[std::move(name)] = std::move(m);
    }

    return true;
}

void server_disk_cache::save_manifest() {
    namespace fs = std::filesystem;

    // mtx must be held. stage the records first so the count written into the
    // header matches the records that follow, then swap the file in place.
    struct staged_record {
        std::string name;
        uint64_t    hash;
        uint64_t    size_file;
        uint64_t    size_main;
        uint64_t    size_drft;
        int64_t     last_used;
        uint64_t    packed_crc;
        std::vector<char> packed;
    };

    std::vector<staged_record> records;
    for (const auto & e : entries) {
        staged_record rec;
        if (!disk_cache_pack_ids(e.tokens, e.media_indices, e.media_token_counts, e.media_ids, rec.packed)) {
            continue;
        }

        rec.name = fs::path(e.path).filename().string();
        if (rec.name.empty() || rec.name.size() > 256) {
            continue;
        }

        rec.hash       = e.hash;
        rec.size_file  = (uint64_t) e.size_file;
        rec.size_main  = (uint64_t) e.size_main;
        rec.size_drft  = (uint64_t) e.size_drft;
        rec.last_used  = e.last_used;
        rec.packed_crc = e.packed_crc;

        records.push_back(std::move(rec));
    }

    const std::string path = dir + "/" + DISK_CACHE_INDEX_NAME;
    const std::string tmp  = path + ".tmp";

    // build the record block first so it can be checksummed as a whole
    std::string body;
    {
        auto append = [&](const void * data, size_t size) {
            body.append(reinterpret_cast<const char *>(data), size);
        };

        const uint32_t n_records = (uint32_t) records.size();
        append(&n_records, sizeof(n_records));

        for (const auto & rec : records) {
            const uint32_t name_len = (uint32_t) rec.name.size();
            append(&name_len, sizeof(name_len));
            append(rec.name.data(), rec.name.size());
            append(&rec.hash, sizeof(rec.hash));
            append(&rec.size_file, sizeof(rec.size_file));
            append(&rec.size_main, sizeof(rec.size_main));
            append(&rec.size_drft, sizeof(rec.size_drft));
            const uint64_t last_used = (uint64_t) rec.last_used;
            append(&last_used, sizeof(last_used));
            append(&rec.packed_crc, sizeof(rec.packed_crc));
            const uint64_t packed_size = (uint64_t) rec.packed.size();
            append(&packed_size, sizeof(packed_size));
            if (!rec.packed.empty()) {
                append(rec.packed.data(), rec.packed.size());
            }
        }
    }

    const uint64_t body_crc = disk_cache_crc64(reinterpret_cast<const uint8_t *>(body.data()), body.size());

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            SRV_WRN("disk cache: cannot open '%s' for writing\n", tmp.c_str());
            return;
        }

        f.write(DISK_CACHE_INDEX_MAGIC, sizeof(DISK_CACHE_INDEX_MAGIC));
        disk_cache_write(f, (uint32_t) DISK_CACHE_INDEX_VERSION);
        disk_cache_write(f, (uint32_t) sig.size());
        f.write(sig.data(), (std::streamsize) sig.size());
        const uint64_t body_size = (uint64_t) body.size();
        disk_cache_write(f, body_size);
        if (!body.empty()) {
            f.write(body.data(), (std::streamsize) body.size());
        }
        disk_cache_write(f, body_crc);

        f.flush();
        if (!f) {
            SRV_ERR("disk cache: failed to write '%s'\n", tmp.c_str());
            std::error_code ec_rm;
            fs::remove(tmp, ec_rm);
            return;
        }
    }

    std::error_code ec;
    fs::remove(path, ec); // the previous index, same as the entry writes
    fs::rename(tmp, path, ec);
    if (ec) {
        SRV_ERR("disk cache: failed to rename '%s': %s\n", tmp.c_str(), ec.message().c_str());
        std::error_code ec_rm;
        fs::remove(tmp, ec_rm);
    }
}

bool server_disk_cache::write_entry(const server_disk_cache_pending & pending, std::string & path_out, size_t & size_out, bool path_owned) {
    namespace fs = std::filesystem;

    const uint64_t hash = disk_cache_hash(pending.ids, pending.media_indices, pending.media_token_counts, pending.media_ids);

    const std::string name = string_format("pc-%016" PRIx64 ".bin", hash);
    const std::string path = dir + "/" + name;
    const std::string tmp  = path + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            SRV_ERR("disk cache: cannot open '%s' for writing\n", tmp.c_str());
            return false;
        }

        f.write(DISK_CACHE_MAGIC, sizeof(DISK_CACHE_MAGIC));
        disk_cache_write(f, (uint32_t) DISK_CACHE_VERSION);
        disk_cache_write(f, (uint32_t) sig.size());
        f.write(sig.data(), (std::streamsize) sig.size());
        disk_cache_write(f, (uint32_t) pending.packed.size());
        if (!pending.packed.empty()) {
            f.write(pending.packed.data(), (std::streamsize) pending.packed.size());
        }
        const auto & ckpts = pending.checkpoints;

        // integrity checksums are computed before the payload is written and
        // stored in the header, so read_entry can verify the bytes it restores.
        // the token/media metadata gets a checksum of its own: it is the only
        // thing that ties the KV payload to a prompt, and a corrupted copy would
        // otherwise be decoded and used to build the restored prompt
        const uint64_t crc_main   = disk_cache_crc64(*pending.main);
        const uint64_t crc_drft   = disk_cache_crc64(*pending.drft);
        const uint64_t crc_packed = disk_cache_crc64(pending.packed);

        disk_cache_write(f, (uint64_t) pending.main->size());
        disk_cache_write(f, (uint64_t) pending.drft->size());
        disk_cache_write(f, (uint32_t) ckpts.size());
        disk_cache_write(f, (uint64_t) disk_cache_now());
        disk_cache_write(f, (uint64_t) disk_cache_now()); // last_used
        disk_cache_write(f, crc_main);
        disk_cache_write(f, crc_drft);
        disk_cache_write(f, crc_packed);

        for (const auto & c : ckpts) {
            disk_cache_write(f, (int64_t)  c->n_tokens);
            disk_cache_write(f, (int32_t)  c->pos_min);
            disk_cache_write(f, (int32_t)  c->pos_max);
            disk_cache_write(f, (uint64_t) c->data_spec.size());
            disk_cache_write(f, (uint64_t) c->data_tgt.size());
            disk_cache_write(f, (uint64_t) c->data_dft.size());
            disk_cache_write(f, (uint64_t) disk_cache_crc64(c->data_spec));
            disk_cache_write(f, (uint64_t) disk_cache_crc64(c->data_tgt));
            disk_cache_write(f, (uint64_t) disk_cache_crc64(c->data_dft));
        }

        if (!pending.main->empty()) {
            f.write(reinterpret_cast<const char *>(pending.main->data()), (std::streamsize) pending.main->size());
        }
        if (!pending.drft->empty()) {
            f.write(reinterpret_cast<const char *>(pending.drft->data()), (std::streamsize) pending.drft->size());
        }
        for (const auto & c : ckpts) {
            if (!c->data_spec.empty()) {
                f.write(reinterpret_cast<const char *>(c->data_spec.data()), (std::streamsize) c->data_spec.size());
            }
            if (!c->data_tgt.empty()) {
                f.write(reinterpret_cast<const char *>(c->data_tgt.data()), (std::streamsize) c->data_tgt.size());
            }
            if (!c->data_dft.empty()) {
                f.write(reinterpret_cast<const char *>(c->data_dft.data()), (std::streamsize) c->data_dft.size());
            }
        }

        f.flush();
        if (!f) {
            f.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            SRV_ERR("disk cache: failed to write '%s'\n", tmp.c_str());
            return false;
        }
    }

    // the caller resolved the ownership of this path under the cache lock
    // (claim_path_locked) and passes the outcome in. without that proof an
    // existing file is never removed: it may belong to a record this process has
    // not classified yet
    if (!path_owned) {
        std::error_code ec_exists;
        if (fs::exists(path, ec_exists)) {
            SRV_ERR("disk cache: refusing to write '%s': the path is not claimed\n", path.c_str());
            std::error_code ec_drop;
            fs::remove(tmp, ec_drop);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // renaming over an existing file fails on windows. the path was claimed,
        // so what is left is an orphan without an `entries` record (a crashed
        // process); removing it is the only way to complete the commit
        std::error_code ec_rm;
        fs::remove(path, ec_rm);
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        SRV_ERR("disk cache: failed to rename '%s': %s\n", tmp.c_str(), ec.message().c_str());
        std::error_code ec_rm;
        fs::remove(tmp, ec_rm);
        return false;
    }

    std::error_code ec_size;
    path_out = path;
    size_out = (size_t) fs::file_size(path, ec_size);

    return true;
}

disk_cache_read_status server_disk_cache::read_entry(const server_disk_cache_entry & e, size_t lcp, server_disk_cache_payload & out) {
    std::ifstream f(e.path, std::ios::binary);
    if (!f) {
        // a missing or locked file is not proof of corruption: keep the entry and
        // let a later request retry (the startup scan removes vanished files)
        SRV_WRN("disk cache: cannot open '%s' for reading\n", e.path.c_str());
        return disk_cache_read_status::io_error;
    }

    char magic[8];
    f.read(magic, sizeof(magic));

    uint32_t version = 0;
    uint32_t sig_len = 0;
    if (!disk_cache_read(f, version) || !disk_cache_read(f, sig_len) ||
            memcmp(magic, DISK_CACHE_MAGIC, sizeof(magic)) != 0 ||
            version != DISK_CACHE_VERSION || sig_len != sig.size()) {
        SRV_WRN("disk cache: '%s' has a mismatching header (v%u, sig %u bytes)\n",
                e.path.c_str(), (unsigned) version, (unsigned) sig_len);
        return disk_cache_read_status::corrupt;
    }

    std::string entry_sig(sig_len, '\0');
    f.read(entry_sig.data(), (std::streamsize) sig_len);
    if (!f || entry_sig != sig) {
        SRV_WRN("disk cache: '%s' was written for a different model or context configuration\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }

    uint32_t packed_size = 0;
    if (!disk_cache_read(f, packed_size) || packed_size > (1u << 28)) {
        SRV_WRN("disk cache: '%s' has a bad prompt metadata size\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }

    out.packed.resize(packed_size);
    if (packed_size > 0) {
        f.read(out.packed.data(), (std::streamsize) out.packed.size());
    }
    if (!f) {
        SRV_WRN("disk cache: '%s' is truncated inside the prompt metadata\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }

    uint64_t size_main   = 0;
    uint64_t size_drft   = 0;
    uint32_t n_ckpt      = 0;
    uint64_t created     = 0;
    uint64_t last_used   = 0;
    uint64_t crc_main    = 0;
    uint64_t crc_drft    = 0;
    uint64_t crc_packed  = 0;
    if (!disk_cache_read(f, size_main) || !disk_cache_read(f, size_drft) ||
            !disk_cache_read(f, n_ckpt) || !disk_cache_read(f, created) ||
            !disk_cache_read(f, last_used) || !disk_cache_read(f, crc_main) ||
            !disk_cache_read(f, crc_drft) || !disk_cache_read(f, crc_packed)) {
        SRV_WRN("disk cache: '%s' is truncated inside the header\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }
    (void) created;

    if (size_main != e.size_main || size_drft != e.size_drft || n_ckpt > 1024) {
        SRV_WRN("disk cache: '%s' does not match its index record (main %" PRIu64 "/%zu, drft %" PRIu64 "/%zu)\n",
                e.path.c_str(), size_main, e.size_main, size_drft, e.size_drft);
        return disk_cache_read_status::corrupt;
    }

    // the packed metadata is validated before anything else is decoded or
    // restored: it decides which prompt the KV payload belongs to. the checksum
    // was written by the writer, so this covers corruption that happened before
    // the entry was indexed - which no read-side hash of the same broken bytes
    // could ever detect
    if (disk_cache_crc64(out.packed) != crc_packed) {
        SRV_WRN("disk cache: prompt metadata checksum mismatch in '%s', dropping it\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }

    // an entry indexed from the startup manifest carries the checksum of the
    // metadata that was verified at scan time: a mismatch means the index and the
    // file disagree, which invalidates the prefix the lookup was based on
    if (e.packed_crc != 0 && e.packed_crc != crc_packed) {
        SRV_WRN("disk cache: '%s' does not match the prompt metadata in the index (crc %016" PRIx64 " vs %016" PRIx64 ")\n",
                e.path.c_str(), crc_packed, e.packed_crc);
        return disk_cache_read_status::corrupt;
    }

    if (!disk_cache_unpack_ids(out.packed, out.tokens, out.media_indices, out.media_token_counts, out.media_ids)) {
        SRV_WRN("disk cache: failed to decode the prompt metadata of '%s'\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }

    // the decoded metadata must describe the entry the lookup selected: the
    // lookup compared prefixes against the indexed token sequence, so a
    // divergence here means the restored KV would not cover the restored prompt
    if (out.tokens != e.tokens ||
            out.media_indices != e.media_indices ||
            out.media_token_counts != e.media_token_counts ||
            out.media_ids != e.media_ids) {
        SRV_WRN("disk cache: '%s' holds a different prompt than the index claims\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }

    struct ckpt_meta {
        int64_t  n_tokens;
        int32_t  pos_min;
        int32_t  pos_max;
        uint64_t size_spec;
        uint64_t size_tgt;
        uint64_t size_dft;
        uint64_t crc_spec;
        uint64_t crc_tgt;
        uint64_t crc_dft;
    };

    std::vector<ckpt_meta> metas(n_ckpt);
    for (auto & m : metas) {
        if (!disk_cache_read(f, m.n_tokens) || !disk_cache_read(f, m.pos_min) || !disk_cache_read(f, m.pos_max) ||
                !disk_cache_read(f, m.size_spec) || !disk_cache_read(f, m.size_tgt) || !disk_cache_read(f, m.size_dft) ||
                !disk_cache_read(f, m.crc_spec) || !disk_cache_read(f, m.crc_tgt) || !disk_cache_read(f, m.crc_dft)) {
            SRV_WRN("disk cache: '%s' is truncated inside the checkpoint table\n", e.path.c_str());
            return disk_cache_read_status::corrupt;
        }
    }

    // only one checkpoint can be used by the restore: the newest one that does not
    // cross the point where the new prompt diverges (or the first one, which starts at pos 0).
    // reading the others would be wasted IO.
    int want = -1;
    for (size_t i = 0; i < metas.size(); i++) {
        if (metas[i].pos_max <= (llama_pos) lcp) {
            want = (int) i;
        }
    }
    if (want < 0 && !metas.empty() && metas[0].pos_min == 0) {
        want = 0;
    }

    const std::streamoff hdr_end = f.tellg();

    out.main.resize(size_main);
    if (size_main > 0) {
        f.read(reinterpret_cast<char *>(out.main.data()), (std::streamsize) size_main);
    }

    out.drft.resize(size_drft);
    if (size_drft > 0) {
        f.read(reinterpret_cast<char *>(out.drft.data()), (std::streamsize) size_drft);
    }

    if (!f) {
        SRV_WRN("disk cache: '%s' is truncated inside the state payload\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }

    // verify the payload before it reaches llama_state_seq_set_data_ext: a torn
    // write or a bad disk sector would otherwise be restored silently and produce
    // wrong inference. a mismatch marks the entry for removal by the caller.
    if (disk_cache_crc64(out.main) != crc_main || disk_cache_crc64(out.drft) != crc_drft) {
        SRV_WRN("disk cache: payload checksum mismatch in '%s', dropping it\n", e.path.c_str());
        return disk_cache_read_status::corrupt;
    }

    out.checkpoints.clear();
    uint64_t ckpt_bytes = 0;
    for (size_t i = 0; i < metas.size(); i++) {
        const auto & m = metas[i];

        if ((int) i != want) {
            f.seekg((std::streamoff) (m.size_spec + m.size_tgt + m.size_dft), std::ios::cur);
            continue;
        }

        ckpt_bytes = m.size_spec + m.size_tgt + m.size_dft;

        auto ckpt = std::make_shared<common_prompt_checkpoint>();

        ckpt->n_tokens = m.n_tokens;
        // not created by a live task: a failed restore must fall back gracefully
        ckpt->id_task  = -1;
        ckpt->pos_min  = m.pos_min;
        ckpt->pos_max  = m.pos_max;

        ckpt->data_spec.resize(m.size_spec);
        if (m.size_spec > 0) {
            f.read(reinterpret_cast<char *>(ckpt->data_spec.data()), (std::streamsize) m.size_spec);
        }
        ckpt->data_tgt.resize(m.size_tgt);
        if (m.size_tgt > 0) {
            f.read(reinterpret_cast<char *>(ckpt->data_tgt.data()), (std::streamsize) m.size_tgt);
        }
        ckpt->data_dft.resize(m.size_dft);
        if (m.size_dft > 0) {
            f.read(reinterpret_cast<char *>(ckpt->data_dft.data()), (std::streamsize) m.size_dft);
        }

        if (!f) {
            SRV_WRN("disk cache: '%s' is truncated inside the checkpoint payload\n", e.path.c_str());
            out.checkpoints.clear();
            return disk_cache_read_status::corrupt;
        }

        // the checkpoint payload feeds llama_state_seq_set_data_ext as well, so
        // it gets the same integrity check as the main/draft states
        if (disk_cache_crc64(ckpt->data_spec) != m.crc_spec ||
                disk_cache_crc64(ckpt->data_tgt) != m.crc_tgt ||
                disk_cache_crc64(ckpt->data_dft) != m.crc_dft) {
            SRV_WRN("disk cache: checkpoint checksum mismatch in '%s', dropping it\n", e.path.c_str());
            out.checkpoints.clear();
            return disk_cache_read_status::corrupt;
        }

        out.checkpoints.push_back(ckpt);
    }

    const uint64_t bytes_read = (uint64_t) hdr_end + size_main + size_drft + ckpt_bytes;
    SRV_TRC("disk cache: read %.2f MiB of %.2f MiB, checkpoints %d/%zu\n",
            bytes_read/1048576.0, e.size_file/1048576.0, want >= 0 ? 1 : 0, metas.size());

    return disk_cache_read_status::ok;
}

bool server_disk_cache::touch_entry(const server_disk_cache_entry & e) {
    // rewrite only the 8-byte last_used field in place: a full copy of a
    // multi-gigabyte entry on every hit would be far too expensive
    std::fstream f(e.path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        return false;
    }

    uint8_t fixed[20]; // magic(8), version(4), sig_len(4), packed_size(4)
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char *>(fixed), sizeof(fixed));
    if (!f) {
        return false;
    }

    const uint32_t sig_len     = *reinterpret_cast<const uint32_t *>(fixed + 12);
    const uint32_t packed_size = *reinterpret_cast<const uint32_t *>(fixed + 16);

    // last_used sits after the variable-length signature and packed metadata and
    // after size_main/size_drft/n_ckpt/created. the offset comes from the same
    // constants the writer uses: a wrong offset overwrites the checksums that
    // follow it, which turns every touched entry into a "damaged" entry that has
    // to be dropped on the next read
    const std::streamoff off_last_used = (std::streamoff) DISK_CACHE_HDR_LAST_USED_OFFSET + (std::streamoff) sig_len + (std::streamoff) packed_size;

    const uint64_t now = (uint64_t) disk_cache_now();
    f.seekp(off_last_used, std::ios::beg);
    f.write(reinterpret_cast<const char *>(&now), sizeof(now));
    f.flush();

    return (bool) f;
}

// remove an entry whose restore can never succeed (or already failed): delete
// the file, drop it from the index and persist the manifest. must not be
// called with mtx held
void server_disk_cache::drop_entry(const std::string & path) {
    std::lock_guard<std::mutex> lock(mtx);

    const disk_cache_remove_status status = remove_file(path);

    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->path != path) {
            continue;
        }

        if (status == disk_cache_remove_status::failed) {
            // the bytes are still on disk: keep them accounted for, but the entry
            // failed validation, so it must not be offered to a lookup again
            quarantine_entry(*it, "could not be removed");
        } else {
            total_size -= std::min(total_size, it->size_file);
            entries.erase(it);
        }
        break;
    }

    // the entry is unusable either way (it failed its checksum or could not be
    // read), so its physical reference is withdrawn even when it stays on disk
    notify_invalidated(path);

    // the entry set changed - persist the startup index
    save_manifest();
}

bool server_disk_cache::covered_cheaply(const disk_cache_prompt_identity & wanted) const {
    // mtx must be held
    for (const auto & e : entries) {
        if (!is_reusable_entry(e)) {
            // a quarantined entry must never be reported as "already covered":
            // the write would be dropped while the lookup cannot use the entry
            continue;
        }

        // shared prefix over tokens *and* media, the tail tolerance, and the
        // read-cost guard - the same deterministic pre-filters load() applies
        if (disk_cache_suppresses_write(identity_of(e), wanted, min_tokens, DISK_CACHE_MAX_PREFIX_LAG_TOKENS)) {
            return true;
        }
    }
    return false;
}

size_t server_disk_cache::drop_prefixes(const llama_tokens & ids, const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids,
        size_t committed_file_bytes, bool replacement_is_admitted) {
    size_t dropped = 0;
    // mtx must be held
    if (!replacement_is_admitted) {
        // the long entry exists on disk but its survival in this capacity
        // transaction is not guaranteed. deleting the shorter snapshots now would
        // make the cache strictly worse if the long one is evicted afterwards, so
        // the decision is postponed instead
        size_t follow_ups = 0;

        for (const auto & e : entries) {
            if (is_reusable_entry(e) && disk_cache_can_replace(
                        [&]() {
                            disk_cache_prompt_identity longer;
                            longer.tokens             = &ids;
                            longer.media_indices      = &media_indices;
                            longer.media_token_counts = &media_token_counts;
                            longer.media_ids          = &media_ids;
                            longer.file_bytes         = committed_file_bytes;
                            return longer;
                        }(), identity_of(e), min_tokens)) {
                follow_ups++;
            }
        }

        if (follow_ups > 0) {
            SRV_TRC("disk cache: keeping %zu superseded snapshot(s): the replacing entry is not admitted yet\n",
                    follow_ups);
        }

        return 0;
    }

    disk_cache_prompt_identity longer;
    longer.tokens             = &ids;
    longer.media_indices      = &media_indices;
    longer.media_token_counts = &media_token_counts;
    longer.media_ids          = &media_ids;
    // the real size of the file that was just admitted, so the cost guard decides
    // with the same number load() will use
    longer.file_bytes         = committed_file_bytes;

    for (auto it = entries.begin(); it != entries.end(); ) {
        // the shorter entry may only go when the longer one can really serve every
        // request it served: media-compatible over that prefix, plus a file that
        // passes the read-cost guard for that prefix length. a plain token prefix
        // is not a substitute proof
        if (!is_reusable_entry(*it) || !disk_cache_can_replace(longer, identity_of(*it), min_tokens)) {
            ++it;
            continue;
        }

        const llama_tokens & old_ids = it->tokens;
        const size_t size_file = std::min(total_size, it->size_file);

        if (remove_file(it->path) == disk_cache_remove_status::failed) {
            // the short snapshot is still on disk and still restorable:
            // keep it, keep its quota and keep its physical reference
            SRV_WRN("disk cache: could not remove the superseded prefix '%s', keeping it\n",
                    it->path.c_str());
            ++it;
            continue;
        }

        SRV_INF("disk cache: dropping '%s': prefix of a stored prompt, %zu tokens, %.2f MiB\n",
                it->path.c_str(), old_ids.size(), size_file/(1024.0*1024.0));
        total_size -= size_file;
        notify_invalidated(it->path);
        it = entries.erase(it);
        dropped++;
    }

    return dropped;
}

void server_disk_cache::dedup_prefixes() {
    // mtx must be held
    namespace fs = std::filesystem;

    // mark every entry that is a redundant prefix of a longer stored entry,
    // then erase the marked ones (erasing while comparing would invalidate
    // the comparison loop)
    std::vector<bool> drop(entries.size(), false);

    for (size_t i = 0; i < entries.size(); ++i) {
        if (drop[i]) {
            continue;
        }
        const auto & a = entries[i];

        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (drop[j]) {
                continue;
            }
            const auto & b = entries[j];

            if (!is_reusable_entry(a) || !is_reusable_entry(b)) {
                continue;
            }

            // the shorter one is only redundant when the longer one can really
            // serve every request it served; the same predicate as drop_prefixes
            const bool a_in_b = disk_cache_can_replace(identity_of(b), identity_of(a), min_tokens);
            const bool b_in_a = disk_cache_can_replace(identity_of(a), identity_of(b), min_tokens);
            if (!a_in_b && !b_in_a) {
                continue;
            }

            const auto & shorter = a_in_b ? a : b;

            const size_t shorter_idx = a_in_b ? i : j;
            if (!drop[shorter_idx]) {
                if (remove_file(entries[shorter_idx].path) == disk_cache_remove_status::failed) {
                    // still on disk and still restorable: leave it in place and do
                    // not pretend the bytes were freed
                    SRV_WRN("disk cache: could not remove the superseded prefix '%s', keeping it\n",
                            entries[shorter_idx].path.c_str());
                    continue;
                }
                SRV_INF("disk cache: dropping '%s': prefix of a stored prompt, %zu tokens, %.2f MiB\n",
                        entries[shorter_idx].path.c_str(), entries[shorter_idx].tokens.size(),
                        entries[shorter_idx].size_file/(1024.0*1024.0));
                total_size -= std::min(total_size, entries[shorter_idx].size_file);
                notify_invalidated(entries[shorter_idx].path);
                drop[shorter_idx] = true;
            }
        }
    }

    for (size_t i = 0; i < entries.size(); ) {
        if (drop[i]) {
            entries.erase(entries.begin() + (long) i);
        } else {
            ++i;
        }
    }
}

bool server_disk_cache::load(server_prompt & prompt, const server_tokens & tokens_new, llama_context * ctx_tgt, llama_context * ctx_dft, int32_t id_slot,
        prompt_cache_restore_info & info) {
    if (!active) {
        return true;
    }

    std::vector<char> packed_new;
    llama_tokens tokens;
    std::vector<uint32_t> media_indices;
    std::vector<uint32_t> media_token_counts;
    std::vector<std::string> media_ids;
    if (!disk_cache_pack(tokens_new, packed_new, tokens, media_indices, media_token_counts, media_ids)) {
        SRV_TRC("%s", "disk cache: request prompt metadata could not be packed, skipping\n");
        return true;
    }

    // the request's identity, shared by every candidate comparison below
    disk_cache_prompt_identity request_identity;
    request_identity.tokens             = &tokens;
    request_identity.media_indices      = &media_indices;
    request_identity.media_token_counts = &media_token_counts;
    request_identity.media_ids          = &media_ids;

    counters.n_lookups++;

    // best lcp over every candidate seen in this call, accepted or not, and the
    // reason the candidate that got furthest was rejected. this separates "no
    // entry shares a prefix" from "a candidate existed but was not restorable"
    size_t              lcp_seen      = 0;
    cache_reject_reason reject_seen   = cache_reject_reason::no_prefix;

    auto note_reject = [&](size_t lcp, cache_reject_reason reason) {
        if (lcp > lcp_seen) {
            lcp_seen    = lcp;
            reject_seen = reason;
        }
    };

    // select the best candidate that has not been rejected yet in this call. the
    // callers of this file must never pick an entry outside the lock, but the
    // multi-gigabyte file IO has to happen without it
    auto pick = [&](const std::vector<std::string> & skip, server_disk_cache_entry & best_out, size_t & best_lcp_out) {
        bool found = false;

        std::lock_guard<std::mutex> lock(mtx);

        SRV_TRC("disk cache: lookup for %zu tokens, %zu entries, min %zu tokens\n", tokens.size(), entries.size(), min_tokens);

        for (const auto & e : entries) {
            if (e.quarantined || std::find(skip.begin(), skip.end(), e.path) != skip.end()) {
                continue;
            }

            const size_t e_size = e.tokens.size();

            // an entry shorter than the minimum can never yield a restorable lcp
            if (e_size < DISK_CACHE_MIN_LOOKUP_TOKENS || e_size < min_tokens) {
                SRV_TRC("disk cache:   - entry with length %7zu skipped: shorter than min %zu tokens\n", e_size, min_tokens);
                note_reject(0, cache_reject_reason::too_short);
                continue;
            }

            // the lcp cannot exceed either length; if that is below the minimum,
            // skip the whole-token comparison
            if (std::min(e_size, tokens.size()) < min_tokens) {
                SRV_TRC("disk cache:   - entry with length %7zu skipped: the prompt is shorter than min %zu tokens\n", e_size, min_tokens);
                note_reject(0, cache_reject_reason::too_short);
                continue;
            }

            // one shared metadata rule, also used by the write-suppression and
            // prefix-dedup paths, so the two can never disagree about what
            // "shares a prefix" means. it never replaces the file validation and
            // the state restore below
            const disk_cache_prefix_match match = disk_cache_match_prefix(identity_of(e), request_identity);
            const size_t lcp             = match.usable_lcp;
            const bool   media_clamped   = !match.media_compatible;

            SRV_TRC("disk cache:   - entry with length %7zu, lcp = %7zu, f_keep = %.3f\n",
                    e_size, lcp, float(lcp) / e_size);

            // unlike the RAM cache there is no eviction cost here: a restore
            // reads the file and keeps every entry, so the f_keep floor does
            // not apply. min_tokens is the only match-quality guard - it keeps
            // the file read cheaper than the prefill it replaces.
            if (lcp < min_tokens) {
                SRV_TRC("disk cache:   - entry with length %7zu skipped: lcp %7zu is below min %zu tokens\n", e_size, lcp, min_tokens);
                note_reject(lcp, media_clamped ? cache_reject_reason::media_mismatch : cache_reject_reason::no_prefix);
                continue;
            }

            // a restore always reads the whole entry file, so skip entries
            // where the bytes read per restored token clearly cost more disk
            // time than the prefill of the shared prefix would take
            if (!disk_cache_cost_ok(e.size_file, lcp)) {
                SRV_TRC("disk cache:   - skipping entry with length %7zu: lcp %7zu would read %.2f MiB per token\n",
                        e_size, lcp, e.size_file/(1024.0*1024.0)/std::max<size_t>(1, lcp));
                note_reject(lcp, cache_reject_reason::io_cost);
                continue;
            }

            // the longest shared prefix wins; on a tie the most recently used
            // entry is fresher and the more similar one keeps more of the cache
            if (!found || lcp > best_lcp_out || (lcp == best_lcp_out && e.last_used > best_out.last_used)) {
                best_lcp_out = lcp;
                best_out     = e;
                found        = true;
            }
        }

        return found;
    };

    // a rejected candidate must not force a full prefill while another entry can
    // still serve part of the prompt: keep trying until the attempts run out or
    // one entry validates. only read_entry failures land here - once KV state was
    // written, the caller has to clear the slot instead
    server_disk_cache_payload payload;
    server_disk_cache_entry   best;
    size_t                    best_lcp = 0;

    std::vector<std::string> rejected;

    bool have = false;

    for (int attempt = 0; attempt < DISK_CACHE_MAX_LOAD_ATTEMPTS; attempt++) {
        best = server_disk_cache_entry();
        best_lcp = 0;

        if (!pick(rejected, best, best_lcp)) {
            if (attempt == 0) {
                SRV_TRC("%s", "disk cache: no candidate shares a long enough prefix\n");

                info.candidate_lcp = lcp_seen;
                info.reject        = reject_seen;
                counters.n_candidate_lcp_tokens += lcp_seen;
                counters.n_reject[(size_t) reject_seen]++;
            }
            return true;
        }

        const float f_keep = float(best_lcp) / best.tokens.size();
        const float f_sim  = float(best_lcp) / std::max<size_t>(1, tokens.size());

        SRV_TRC("disk cache: candidate '%s' with %zu tokens, f_keep = %.3f, f_sim = %.3f\n",
                best.path.c_str(), best.tokens.size(), f_keep, f_sim);

        payload = server_disk_cache_payload();

        const int64_t t_read = ggml_time_us();

        const disk_cache_read_status status = read_entry(best, best_lcp, payload);
        if (status == disk_cache_read_status::ok) {
            info.read_bytes += best.size_file;
            info.elapsed_us += ggml_time_us() - t_read;
            have = true;
            break;
        }

        rejected.push_back(best.path);

        if (status == disk_cache_read_status::corrupt) {
            SRV_WRN("disk cache: dropping the corrupt entry '%s'\n", best.path.c_str());
            drop_entry(best.path);
            info.reject = cache_reject_reason::checksum_error;
        } else {
            SRV_WRN("disk cache: keeping '%s' after a read error, a later request may succeed\n", best.path.c_str());
            info.reject = cache_reject_reason::read_error;
        }
    }

    if (!have) {
        SRV_WRN("disk cache: no restorable entry left for %zu tokens after %d attempt(s), prefilling\n",
                tokens.size(), DISK_CACHE_MAX_LOAD_ATTEMPTS);

        info.candidate_lcp = std::max(info.candidate_lcp, lcp_seen);
        if (info.reject == cache_reject_reason::none) {
            info.reject = reject_seen;
        }
        counters.n_candidate_lcp_tokens += info.candidate_lcp;
        counters.n_reject[(size_t) info.reject]++;
        return true;
    }

    counters.n_candidate_lcp_tokens += best_lcp;

    const int64_t t_start = ggml_time_us();

    // everything below this point is metadata-only until the state restore, so a
    // rejection here is still a clean fallback: nothing was written to the contexts

    // the media metadata of the entry must match the request at exactly the same
    // positions, otherwise the cached KV does not describe the restored prompt
    try {
        for (size_t i = 0; i < payload.media_indices.size(); ++i) {
            const uint32_t idx = payload.media_indices[i];
            if (idx >= best_lcp || idx >= tokens_new.size() ||
                    tokens_new.media_chunk_token_count(idx) != payload.media_token_counts[i] ||
                    tokens_new.media_chunk_id(idx) != payload.media_ids[i]) {
                throw std::runtime_error("cached media metadata differs from the current prompt");
            }
        }
    } catch (const std::exception & err) {
        // deterministic media mismatch - drop the entry, it can never match again
        SRV_WRN("disk cache: cannot use '%s': %s, dropping it\n", best.path.c_str(), err.what());
        info.reject = cache_reject_reason::media_mismatch;
        counters.n_reject[(size_t) info.reject]++;
        drop_entry(best.path);
        return true;
    }

    const size_t size_main = payload.main.size();
    const size_t size_drft = payload.drft.size();

    if (size_drft > 0 && !ctx_dft) {
        // deterministic: this process has no draft context, the entry can never
        // be restored here - drop it instead of failing on every matching request
        SRV_WRN("%s", "disk cache: entry has a draft state but there is no draft context, dropping it\n");
        info.reject = cache_reject_reason::signature_mismatch;
        counters.n_reject[(size_t) info.reject]++;
        drop_entry(best.path);
        return true;
    }

    // from here on the KV cells are being replaced with the entry's data while
    // slot.prompt still holds the previous tokens: any failure must return
    // false so the caller clears the slot and prefills from scratch. returning
    // true would leave KV and prompt.tokens mismatched -> silent wrong inference
    if (llama_state_seq_set_data_ext(ctx_tgt, payload.main.data(), size_main, id_slot, 0) != size_main) {
        // keep the entry: the failure can be transient (e.g. VRAM pressure),
        // and a retry on the next request may succeed
        SRV_WRN("disk cache: failed to restore the target state (size %zu)\n", size_main);
        info.reject = cache_reject_reason::restore_failed;
        counters.n_reject[(size_t) info.reject]++;
        return false;
    }

    if (size_drft > 0) {
        if (llama_state_seq_set_data_ext(ctx_dft, payload.drft.data(), size_drft, id_slot, 0) != size_drft) {
            SRV_WRN("disk cache: failed to restore the draft state (size %zu)\n", size_drft);
            info.reject = cache_reject_reason::restore_failed;
            counters.n_reject[(size_t) info.reject]++;
            return false;
        }
    }

    try {
        // only the shared prefix (best_lcp tokens) was restored from disk: the
        // remainder of the cached prompt diverges from the current one and must
        // not be kept, otherwise the media-placeholder fill would compare tokens
        // past the prefix and reject the restore
        llama_tokens restored_tokens = payload.tokens;
        if (best_lcp < restored_tokens.size()) {
            restored_tokens.resize(best_lcp);
        }

        // note: the KV cells now hold the whole entry (best.tokens.size()
        // tokens) while prompt.tokens keeps only best_lcp - this intermediate
        // state is resolved later by update_slots' seq_rm(id, p0, -1) tail trim
        // and by the pos_max <= pos_next checkpoint guard. do not remove those
        // two, or the KV/prompt accounting breaks
        prompt.tokens = server_tokens(restored_tokens, tokens_new.has_mtmd);
        prompt.tokens.append_media_placeholders_from(tokens_new);
    } catch (const std::exception & err) {
        // the state was already restored, so the caller must reset the slot
        SRV_WRN("disk cache: failed to rebuild the prompt of '%s': %s\n", best.path.c_str(), err.what());
        info.reject = cache_reject_reason::restore_failed;
        counters.n_reject[(size_t) info.reject]++;
        return false;
    }

    prompt.checkpoints = std::move(payload.checkpoints);

    const int64_t now = disk_cache_now();

    {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto & e : entries) {
            if (e.path == best.path) {
                e.last_used = now;
                e.last_used_dirty = true;
                break;
            }
        }
    }

    // last_used is updated in memory only on a hit; persisting it here would do
    // an 8-byte random write per restore and invalidate the file's page cache.
    // dirty timestamps are flushed in batch on shutdown (and a forced kill
    // simply falls back to creation-time LRU order after a restart).

    // note: restored = best_lcp (the usable prefix), not the whole entry -
    // printing the entry length here would make short-prefix hits of long
    // entries look like full restores and corrupt any bpt analysis
    info.src          = prompt_cache_restore_info::source::disk;
    info.entry_tokens = best.tokens.size();
    info.candidate_lcp = best_lcp;
    info.lcp          = best_lcp;
    info.elapsed_us  += ggml_time_us() - t_start;

    counters.n_read_bytes += info.read_bytes;
    counters.n_restore_us += info.elapsed_us;

    SRV_DBG("disk cache: restored %zu/%zu tokens (%zu + %zu bytes) in %.2f ms\n",
            best_lcp, best.tokens.size(), size_main, size_drft, (ggml_time_us() - t_start)/1000.0);

    return true;
}

void server_disk_cache::store(server_prompt_cache_state && state) {
    if (!active) {
        return;
    }

    if (state.prompt.tokens.size() < min_tokens) {
        counters.n_enqueue_rejected++;
        return;
    }

    server_disk_cache_pending pending;

    if (!disk_cache_pack(state.prompt.tokens, pending.packed, pending.ids,
                pending.media_indices, pending.media_token_counts, pending.media_ids)) {
        SRV_TRC("%s", "disk cache: prompt metadata cannot be packed, skipping\n");
        counters.n_enqueue_rejected++;
        return;
    }

    if (pending.ids.size() < min_tokens) {
        counters.n_enqueue_rejected++;
        return;
    }

    // checkpoints travel with the entry: hybrid/recurrent models need them to reuse a long prefix
    pending.bytes = state.size();

    if (limit_size > 0 && pending.bytes > limit_size) {
        SRV_WRN("disk cache: entry of %.2f MiB exceeds the %.2f MiB limit, skipping\n",
                pending.bytes/(1024.0*1024.0), limit_size/(1024.0*1024.0));
        counters.n_enqueue_rejected++;
        return;
    }

    const uint64_t hash = disk_cache_hash(pending.ids, pending.media_indices, pending.media_token_counts, pending.media_ids);

    disk_cache_prompt_identity pending_identity;
    pending_identity.tokens             = &pending.ids;
    pending_identity.media_indices      = &pending.media_indices;
    pending_identity.media_token_counts = &pending.media_token_counts;
    pending_identity.media_ids          = &pending.media_ids;
    pending_identity.file_bytes         = pending.bytes;

    {
        std::lock_guard<std::mutex> lock(mtx);

        // the path is derived from the hash, so a hash hit is a path hit: resolve
        // the owner first and only then decide between "already stored" and
        // "conflict". a hash hit with a different identity - including a different
        // token length - is a conflict, not something to skip
        for (const auto & e : entries) {
            if (e.hash != hash) {
                continue;
            }

            if (is_reusable_entry(e) &&
                    disk_cache_classify_duplicate(identity_of(e), pending_identity, true) == disk_cache_duplicate::identical) {
                SRV_TRC("disk cache: %016" PRIx64 " is already stored, skipping\n", hash);
                counters.n_enqueue_dedup++;
                return;
            }

            if (!is_reusable_entry(e)) {
                // a quarantined holder is not "already stored": the entry cannot
                // serve a lookup, so counting this as a duplicate would keep the
                // prompt permanently uncacheable. the worker resolves the path
                // before writing (reclaim or refuse)
                SRV_TRC("disk cache: %016" PRIx64 " is held by a quarantined entry, resolving the path at write time\n", hash);
                continue;
            }

            counters.n_hash_collisions++;
            counters.n_path_conflicts++;
            SRV_ERR("disk cache: %016" PRIx64 " is held by '%s' with a different token/media identity, refusing the write\n",
                    hash, e.path.c_str());
            return;
        }

        if (inflight.count(hash)) {
            SRV_TRC("disk cache: %016" PRIx64 " is already queued, skipping\n", hash);
            counters.n_enqueue_dedup++;
            return;
        }

        if (covered_cheaply(pending_identity)) {
            SRV_INF("disk cache: %zu tokens are covered up to a %zu-token tail by a stored entry, skipping a %.2f MiB write\n",
                    pending.ids.size(), DISK_CACHE_MAX_PREFIX_LAG_TOKENS, pending.bytes/(1024.0*1024.0));
            counters.n_enqueue_dedup++;
            return;
        }

        if ((queue_bytes_max > 0 && queue_bytes + pending.bytes > queue_bytes_max) ||
                queue.size() >= DISK_CACHE_QUEUE_MAX_ENTRIES) {
            SRV_WRN("disk cache: write queue is full (%zu entries, %.2f MiB pending), skipping a prompt with %zu tokens\n",
                    queue.size(), queue_bytes/(1024.0*1024.0), pending.ids.size());
            counters.n_enqueue_rejected++;
            return;
        }

        // the RAM copy is being dropped: take over its buffers. moving the
        // shared pointers is free; the state is destroyed by the caller right
        // after store() returns
        pending.main        = std::move(state.data.main);
        pending.drft        = std::move(state.data.drft);
        pending.checkpoints = state.prompt.checkpoints;

        pending.hash    = hash;
        queue_bytes    += pending.bytes;
        inflight.insert(hash);
        queue.push_back(std::move(pending));

        counters.n_enqueue_queued++;
    }

    cv.notify_one();
}

bool server_disk_cache::mirror(const server_prompt_cache_state & state, size_t budget_left) {
    if (!active) {
        return false;
    }

    if (state.prompt.tokens.size() < min_tokens) {
        counters.n_enqueue_rejected++;
        return false;
    }

    const size_t bytes = state.size();

    if (limit_size > 0 && bytes > limit_size) {
        counters.n_enqueue_rejected++;
        return false;
    }

    if (budget_left < bytes) {
        counters.n_enqueue_rejected++;
        return false;
    }

    server_disk_cache_pending pending;

    if (!disk_cache_pack(state.prompt.tokens, pending.packed, pending.ids,
                pending.media_indices, pending.media_token_counts, pending.media_ids)) {
        counters.n_enqueue_rejected++;
        return false;
    }

    if (pending.ids.size() < min_tokens) {
        counters.n_enqueue_rejected++;
        return false;
    }

    pending.bytes = bytes;

    const uint64_t hash = disk_cache_hash(pending.ids, pending.media_indices, pending.media_token_counts, pending.media_ids);

    disk_cache_prompt_identity pending_identity;
    pending_identity.tokens             = &pending.ids;
    pending_identity.media_indices      = &pending.media_indices;
    pending_identity.media_token_counts = &pending.media_token_counts;
    pending_identity.media_ids          = &pending.media_ids;
    pending_identity.file_bytes         = pending.bytes;

    {
        std::lock_guard<std::mutex> lock(mtx);

        // already written, or already waiting in the queue
        for (const auto & e : entries) {
            if (e.hash != hash) {
                continue;
            }

            if (is_reusable_entry(e) &&
                    disk_cache_classify_duplicate(identity_of(e), pending_identity, true) == disk_cache_duplicate::identical) {
                counters.n_enqueue_dedup++;
                return false;
            }

            if (!is_reusable_entry(e)) {
                // see store(): a quarantined holder is not a stored duplicate
                continue;
            }

            counters.n_hash_collisions++;
            counters.n_path_conflicts++;
            SRV_ERR("disk cache: %016" PRIx64 " is held by '%s' with a different token/media identity, refusing the mirror\n",
                    hash, e.path.c_str());
            return false;
        }
        if (inflight.count(hash)) {
            counters.n_enqueue_dedup++;
            return false;
        }

        if (covered_cheaply(pending_identity)) {
            SRV_INF("disk cache: %zu tokens are covered up to a %zu-token tail by a stored entry, skipping a %.2f MiB mirror\n",
                    pending.ids.size(), DISK_CACHE_MAX_PREFIX_LAG_TOKENS, pending.bytes/(1024.0*1024.0));
            counters.n_enqueue_dedup++;
            return false;
        }

        if ((queue_bytes_max > 0 && queue_bytes + pending.bytes > queue_bytes_max) ||
                queue.size() >= DISK_CACHE_QUEUE_MAX_ENTRIES) {
            counters.n_enqueue_rejected++;
            return false;
        }

        // the entry stays in RAM: share the state buffers instead of copying
        // gigabytes of KV data (the buffers stay alive until the write is
        // done); the checkpoints are immutable and shared as before
        pending.main        = state.data.main;
        pending.drft        = state.data.drft;
        pending.checkpoints = state.prompt.checkpoints;

        pending.hash    = hash;
        queue_bytes    += pending.bytes;
        inflight.insert(hash);
        queue.push_back(std::move(pending));

        counters.n_enqueue_queued++;
    }

    cv.notify_one();

    return true;
}

void server_disk_cache::sweep_expired() {
    sweep_expired({});
}

void server_disk_cache::sweep_expired(const disk_cache_protected_paths & protected_paths) {
    namespace fs = std::filesystem;

    // mtx must be held
    if (ttl_seconds <= 0) {
        return;
    }

    const int64_t now = disk_cache_now();

    for (auto it = entries.begin(); it != entries.end(); ) {
        // the TTL pass runs before eviction: it must not take the entry this
        // transaction is trying to admit, however idle it looks
        if (is_protected(*it, protected_paths)) {
            ++it;
            continue;
        }

        if (now - it->last_used > ttl_seconds) {
            const disk_cache_remove_status status = remove_file(it->path);

            if (status == disk_cache_remove_status::failed) {
                // expired, but the bytes are still there: keep the quota honest
                // and stop offering it to a lookup
                quarantine_entry(*it, "expired but could not be removed");
                notify_invalidated(it->path);
                ++it;
                continue;
            }

            SRV_INF("disk cache: dropping '%s': expired after %lld s idle (%.2f MiB)\n",
                    it->path.c_str(), (long long) (now - it->last_used), it->size_file/(1024.0*1024.0));

            total_size -= std::min(total_size, it->size_file);
            notify_invalidated(it->path);
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<disk_cache_quota_item> server_disk_cache::quota_items_locked(const server_disk_cache_entry * keep_new,
        std::vector<std::string> & path_storage) const {
    // mtx must be held. paths are stored in `path_storage` so the plan can hold
    // stable pointers while the caller mutates `entries`
    std::vector<disk_cache_quota_item> items;
    items.reserve(entries.size() + (keep_new != nullptr ? 1 : 0));

    path_storage.reserve(entries.size() + 1);

    const auto append = [&](const server_disk_cache_entry & e, bool is_new) {
        path_storage.push_back(e.path);

        disk_cache_quota_item item;
        item.path        = &path_storage.back();
        item.size_file   = e.size_file;
        item.last_used   = e.last_used;
        item.quarantined = e.quarantined;
        // the provisional entry is protected by construction: it is the one whose
        // admission the whole transaction is about
        item.protected_  = is_new;
        items.push_back(item);
    };

    for (const auto & e : entries) {
        if (keep_new != nullptr && &e == keep_new) {
            continue; // appended below as the provisional entry
        }
        append(e, false);
    }

    if (keep_new != nullptr) {
        append(*keep_new, true);
    }

    return items;
}

bool server_disk_cache::is_protected(const server_disk_cache_entry & entry,
        const disk_cache_protected_paths & protected_paths) {
    // keyed by path, not by address: `entries` is a vector and every erase() moves
    // the elements behind the removed one, so a stored pointer or iterator would
    // silently start referring to a different entry
    return protected_paths.find(entry.path) != protected_paths.end();
}

void server_disk_cache::enforce_limit() {
    enforce_limit({});
}

void server_disk_cache::enforce_limit(const disk_cache_protected_paths & protected_paths) {
    namespace fs = std::filesystem;

    // mtx must be held. the TTL pass gets the same protection set: it runs first
    // and its erase() calls are what would otherwise invalidate a pointer-based
    // keep list
    sweep_expired(protected_paths);

    // entries that could not be deleted are quarantined and skipped for the rest
    // of this pass: picking the same unremovable file again would loop forever
    while (limit_size > 0 && total_size > limit_size && !entries.empty()) {
        auto it = entries.end();

        for (auto cur = entries.begin(); cur != entries.end(); ++cur) {
            if (cur->quarantined) {
                continue;
            }
            // re-checked on every iteration, after every erase: the protection is a
            // property of the path, so it stays correct as the vector shifts
            if (is_protected(*cur, protected_paths)) {
                continue; // this transaction must not lose it
            }
            if (it == entries.end() || cur->last_used < it->last_used) {
                it = cur;
            }
        }

        if (it == entries.end()) {
            // nothing droppable left (everything is quarantined or protected): stop
            // instead of spinning, and leave the over-quota state visible
            SRV_WRN("disk cache: over the %.2f MiB limit but no removable entry left (%.2f MiB used)\n",
                    limit_size/(1024.0*1024.0), total_size/(1024.0*1024.0));
            break;
        }

        const disk_cache_remove_status status = remove_file(it->path);

        if (status == disk_cache_remove_status::failed) {
            quarantine_entry(*it, "over quota but could not be removed");
            notify_invalidated(it->path);
            continue;
        }

        SRV_WRN("disk cache: limit reached, removing '%s' (%.2f MiB)\n",
                it->path.c_str(), it->size_file/(1024.0*1024.0));

        total_size -= std::min(total_size, it->size_file);
        notify_invalidated(it->path);
        entries.erase(it);
    }
}

void server_disk_cache::worker() {
    size_t bytes_since_yield = 0;

    for (;;) {
        server_disk_cache_pending pending;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]() { return stop_flag || !queue.empty(); });

            if (queue.empty()) {
                if (stop_flag) {
                    return;
                }
                continue;
            }

            pending = std::move(queue.front());
            queue.pop_front();
        }

        const size_t n_tokens = pending.ids.size();
        const size_t bytes    = pending.bytes;
        const uint64_t hash   = pending.hash;

        const int64_t t_start = ggml_time_us();

        std::string path;
        size_t size_file = 0;

        // skip the write when a stored entry can actually replace this prompt:
        // a longer entry whose shared prefix is media-compatible over the whole
        // prompt *and* whose file stays within the read-cost guard. a plain token
        // prefix is not enough - a huge entry cannot economically serve a short
        // request, and a quarantined one cannot serve anything
        bool redundant = false;
        {
            disk_cache_prompt_identity pending_identity;
            pending_identity.tokens             = &pending.ids;
            pending_identity.media_indices      = &pending.media_indices;
            pending_identity.media_token_counts = &pending.media_token_counts;
            pending_identity.media_ids          = &pending.media_ids;
            pending_identity.file_bytes         = bytes;

            std::lock_guard<std::mutex> lock(mtx);
            for (const auto & e : entries) {
                if (!is_reusable_entry(e)) {
                    continue;
                }
                if (disk_cache_can_replace(identity_of(e), pending_identity, min_tokens)) {
                    redundant = true;
                    break;
                }
            }
        }

        if (redundant) {
            std::lock_guard<std::mutex> lock(mtx);
            queue_bytes -= std::min(queue_bytes, bytes);
            inflight.erase(hash);
            SRV_TRC("disk cache: skipping %zu tokens, already covered by a stored prompt\n", n_tokens);
            continue;
        }

        // resolve the ownership of the target path before writing anything. the
        // name is the content hash, so a quarantined or foreign entry may already
        // hold it; the writer may not overwrite either of them
        bool path_ok = false;
        {
            const_cast<server_disk_cache *>(this)->path_owner_scratch = disk_cache_path_owner::free;

            std::lock_guard<std::mutex> lock(mtx);

            disk_cache_prompt_identity pending_identity;
            pending_identity.tokens             = &pending.ids;
            pending_identity.media_indices      = &pending.media_indices;
            pending_identity.media_token_counts = &pending.media_token_counts;
            pending_identity.media_ids          = &pending.media_ids;
            pending_identity.file_bytes         = bytes;

            const std::string target = dir + "/" + string_format("pc-%016" PRIx64 ".bin", hash);
            path_owner_scratch = claim_path_locked(hash, target, pending_identity, "write");

            if (path_owner_scratch == disk_cache_path_owner::identical) {
                // the very same prompt is already stored and usable
                queue_bytes -= std::min(queue_bytes, bytes);
                inflight.erase(hash);
                counters.n_enqueue_dedup++;
                SRV_TRC("disk cache: %016" PRIx64 " is already stored, skipping\n", hash);
                continue;
            }

            path_ok = path_owner_scratch == disk_cache_path_owner::free;
        }

        if (!path_ok) {
            std::lock_guard<std::mutex> lock(mtx);
            queue_bytes -= std::min(queue_bytes, bytes);
            inflight.erase(hash);
            counters.n_enqueue_rejected++;
            continue;
        }

        const bool ok = write_entry(pending, path, size_file, /*path_owned=*/true);

        {
            std::lock_guard<std::mutex> lock(mtx);

            queue_bytes -= std::min(queue_bytes, bytes);
            inflight.erase(hash);

            if (ok) {
                server_disk_cache_entry committed;
                bool                    have_committed = false;

                server_disk_cache_entry e;
                e.path      = path;
                e.hash      = hash;
                e.tokens    = pending.ids;
                e.media_indices = pending.media_indices;
                e.media_token_counts = pending.media_token_counts;
                e.media_ids = pending.media_ids;
                e.size_main = pending.main->size();
                e.size_drft = pending.drft->size();
                e.size_file = size_file;
                e.last_used = disk_cache_now();
                // same checksum the writer put into the file header: it binds the
                // index record to the file it describes
                e.packed_crc = disk_cache_crc64(pending.packed);

                // admission first, replacement second.
                //
                // the file is on disk, but that alone does not mean it will
                // survive: the quota pass may evict it right away. deleting the
                // shorter snapshots it supersedes before knowing that would trade
                // a working snapshot for one that is about to disappear.
                //
                // so: plan the capacity transaction with the new entry protected,
                // reclaim only what is genuinely reclaimable, and only then decide
                // about the superseded short snapshots
                std::vector<std::string> path_storage;

                std::vector<disk_cache_quota_item> plan_in = quota_items_locked(&e, path_storage);
                disk_cache_quota_plan              plan    = disk_cache_plan_quota(plan_in, limit_size);

                if (!plan.fits) {
                    // even after reclaiming everything reclaimable the new entry
                    // does not fit. roll it back instead of deleting snapshots for
                    // a file that cannot stay
                    SRV_WRN("disk cache: '%s' (%.2f MiB) does not fit the %.2f MiB limit with %.2f MiB protected, rolling it back\n",
                            path.c_str(), size_file/(1024.0*1024.0),
                            limit_size/(1024.0*1024.0), plan.protected_bytes/(1024.0*1024.0));

                    const disk_cache_remove_status rolled_back = remove_file(path);

                    counters.n_write_rejected++;

                    if (rolled_back == disk_cache_remove_status::failed) {
                        // the rollback did not free anything: the bytes are still on
                        // disk. pretending the write never happened would leave an
                        // untracked file occupying space, so it becomes a
                        // quarantined entry instead - accounted for, and excluded
                        // from every reuse path
                        SRV_ERR("disk cache: could not roll back '%s', keeping it as a quarantined entry (%.2f MiB)\n",
                                path.c_str(), size_file/(1024.0*1024.0));

                        // the path was claimed before the write, so the new entry
                        // is its only owner; registering it here keeps the ledger
                        // and the directory consistent
                        quarantine_entry(e, "write rejected but the rollback failed");

                        total_size += size_file;
                        entries.push_back(std::move(e));

                        counters.n_rollback_failed++;
                    }
                } else {
                    total_size += size_file;
                    entries.push_back(std::move(e));

                    // protect the new entry by path, not by address: `entries` is a
                    // vector, so a pointer into it goes stale as soon as the quota
                    // pass erases anything ahead of the new entry
                    disk_cache_protected_paths protected_paths = { path };

                    // reclaim the planned candidates. an entry that refuses to be
                    // deleted is quarantined by the quota pass and its bytes stay
                    // accounted for
                    enforce_limit(protected_paths);

                    // `plan.fits` says the plan was satisfiable *if every candidate
                    // could really be removed*. the pass above may have failed to
                    // remove some of them, so ask the entry set what actually
                    // happened rather than trusting the plan
                    bool admitted = false;
                    for (const auto & cur : entries) {
                        if (cur.path == path && cur.quarantined == false) {
                            admitted = true;
                            break;
                        }
                    }

                    if (!admitted) {
                        SRV_WRN("disk cache: '%s' was evicted during its own admission pass, not using it as a replacement\n",
                                path.c_str());
                    }

                    // the new entry is really there and usable - only now may it
                    // justify deleting the snapshots it supersedes
                    const size_t superseded = drop_prefixes(pending.ids, pending.media_indices,
                            pending.media_token_counts, pending.media_ids,
                            size_file, /*replacement_is_admitted=*/admitted);

                    // the plan was only a plan. if the physical pass could not free
                    // what it counted on, the ledger is now over the limit
                    if (limit_size > 0 && total_size > limit_size) {
                        if (superseded == 0) {
                            // nothing this transaction deleted is required for
                            // recovery, and the only reason the quota is still
                            // unmet is a failed removal. undo the whole write
                            // instead of keeping a file that does not fit
                            SRV_WRN("disk cache: quota still exceeded after the admission pass (%.2f MiB over), "
                                    "rolling back '%s' because no superseded snapshot was deleted\n",
                                    (total_size - limit_size)/(1024.0*1024.0), path.c_str());

                            const disk_cache_remove_status rolled_back = remove_file(path);

                            counters.n_write_rejected++;
                            counters.n_quota_unreclaimed++;

                            if (rolled_back == disk_cache_remove_status::failed) {
                                SRV_ERR("disk cache: could not roll back '%s', keeping it as a quarantined entry (%.2f MiB)\n",
                                        path.c_str(), size_file/(1024.0*1024.0));

                                for (auto & cur : entries) {
                                    if (cur.path == path) {
                                        quarantine_entry(cur, "quota unmet and the rollback failed");
                                        break;
                                    }
                                }

                                counters.n_rollback_failed++;
                            } else {
                                total_size -= std::min(total_size, size_file);

                                for (auto it = entries.begin(); it != entries.end(); ++it) {
                                    if (it->path == path) {
                                        notify_invalidated(it->path);
                                        entries.erase(it);
                                        break;
                                    }
                                }
                            }
                        } else {
                            // this transaction already removed snapshots that only
                            // the new entry replaces. deleting that replacement now
                            // would leave the cache strictly worse than before, so
                            // the over-quota state is kept visible instead
                            SRV_WRN("disk cache: over the %.2f MiB limit by %.2f MiB after replacing %zu snapshot(s); "
                                    "keeping the ledger honest and refusing further uncontrolled writes\n",
                                    limit_size/(1024.0*1024.0), (total_size - limit_size)/(1024.0*1024.0), superseded);

                            counters.n_quota_unreclaimed++;
                        }
                    }
                }

                // the entry set changed - persist the startup index
                save_manifest();

                // only now is the file committed and discoverable. the quota pass
                // and drop_prefixes above may have removed it again, so look it up
                // instead of assuming it is the last entry. the notification is
                // delivered here, inside the lock and in the same critical section
                // that changed the entry set, so it can never arrive after a later
                // withdrawal of the same path
                //
                // "still in entries" is not the question, though: a rejected write
                // whose rollback failed is kept for accounting only and must never
                // reach the prefix directory, or the directory would hold a
                // persisted reference to a snapshot load() refuses to use - one
                // that also vanishes on restart, since startup registration skips
                // quarantined entries
                std::vector<disk_cache_commit_candidate> commit_view;
                commit_view.reserve(entries.size());

                for (const auto & cur : entries) {
                    if (cur.path == path) {
                        commit_view.push_back({ cur.path, cur.quarantined, !cur.tokens.empty() });
                    }
                }

                // one predicate, shared with the unit tests: present, not
                // quarantined, non-empty. the record to publish is this very entry
                if (disk_cache_commit_eligible(commit_view, path)) {
                    for (const auto & cur : entries) {
                        if (cur.path == path && is_reusable_entry(cur)) {
                            committed      = cur;
                            have_committed = true;
                            break;
                        }
                    }
                } else if (!commit_view.empty()) {
                    // the path is here, but only as accounted bytes: state it
                    // instead of silently looking like a committed snapshot
                    SRV_WRN("disk cache: '%s' stays accounted for but is not usable, not publishing a persisted reference\n",
                            path.c_str());
                }

                if (have_committed) {
                    notify_committed(committed);
                }
            }
        }

        if (ok) {
            counters.n_write_bytes += size_file;

            SRV_INF("disk cache: stored %zu tokens, %.2f MiB in %.2f ms\n",
                    n_tokens, size_file/(1024.0*1024.0), (ggml_time_us() - t_start)/1000.0);

            // throttle a burst of large writes so the disk stays responsive for
            // inference; only kicks in once a chunk's worth has been written
            bytes_since_yield += size_file;
            if (bytes_since_yield >= DISK_CACHE_WRITE_THROTTLE_CHUNK) {
                bytes_since_yield = 0;
                std::this_thread::sleep_for(std::chrono::microseconds(DISK_CACHE_WRITE_THROTTLE_US));
            }
        }
    }
}

void server_prompt_cache::init_disk(const std::string & path, const std::string & sig, int32_t limit_size_mib, int32_t min_tokens, int32_t ttl_seconds, int32_t queue_max_mib) {
    disk = std::make_unique<server_disk_cache>(path, sig, limit_size_mib, min_tokens, ttl_seconds, queue_max_mib);
}

void server_prompt_cache::spill(server_prompt_cache_state & state) {
    if (disk) {
        disk->store(std::move(state));
    }
}

void server_prompt_cache::flush_to_disk() {
    if (!disk || !disk->active || states.empty()) {
        return;
    }

    // the newest entries are the ones a returning session is most likely to reuse.
    // mirroring copies the state (the RAM entry stays), so the extra memory is bounded:
    // a quarter of the RAM cache, at least 1.5 GiB and at most 6 GiB
    size_t budget = 1536ull*1024ull*1024ull;
    if (limit_size > 0) {
        budget = std::max<size_t>(budget, limit_size/4);
    }
    budget = std::min<size_t>(budget, 6ull*1024ull*1024ull*1024ull);

    size_t n_flushed = 0;
    for (auto it = states.rbegin(); it != states.rend(); ++it) {
        const size_t bytes = it->size();

        if (bytes > budget) {
            SRV_TRC("disk cache: entry of %.2f MiB is too large to mirror, it will be spilled on eviction\n",
                    bytes/(1024.0*1024.0));
            continue;
        }

        if (disk->mirror(*it, budget)) {
            budget -= bytes;
            n_flushed++;
        }
    }

    if (n_flushed > 0) {
        SRV_INF("disk cache: mirrored %zu prompt(s) from the RAM cache\n", n_flushed);
    }
}

bool server_prompt_cache::materialize(const server_prompt & prompt, llama_context * ctx_tgt, llama_context * ctx_dft,
        int32_t id_slot, server_prompt_cache_state & out) {
    // not gated on this tier's own size policy: a prefix may exceed the RAM
    // budget and still be worth its own disk object. the disk tier owns the
    // decision that matters here - identity, dedup, read-cost and quota
    if (!disk || !ctx_tgt) {
        return false;
    }

    if (prompt.tokens.size() == 0) {
        return false;
    }

    const size_t size_tgt = llama_state_seq_get_size_ext(ctx_tgt, id_slot, LLAMA_STATE_SEQ_FLAGS_NONE);
    const size_t size_dft = ctx_dft ? llama_state_seq_get_size_ext(ctx_dft, id_slot, LLAMA_STATE_SEQ_FLAGS_NONE) : 0;

    out.prompt.tokens      = prompt.tokens.clone();
    out.prompt.checkpoints = prompt.checkpoints;

    out.data.main = std::make_shared<common_state_data>();
    out.data.drft = std::make_shared<common_state_data>();

    try {
        out.data.main->resize(size_tgt);
        out.data.drft->resize(size_dft);
    } catch (const std::bad_alloc & e) {
        SRV_ERR("disk cache: cannot allocate %.2f MiB for a prefix snapshot: %s\n",
                (size_tgt + size_dft)/(1024.0*1024.0), e.what());
        return false;
    }

    // sizes are taken first and the written counts are checked against them: a
    // short write would produce a file whose claimed token range has no complete
    // state behind it
    const size_t n_tgt = llama_state_seq_get_data_ext(ctx_tgt,
            out.data.main->data(), out.data.main->size(), id_slot, LLAMA_STATE_SEQ_FLAGS_NONE);
    const size_t n_dft = ctx_dft ? llama_state_seq_get_data_ext(ctx_dft,
            out.data.drft->data(), out.data.drft->size(), id_slot, LLAMA_STATE_SEQ_FLAGS_NONE) : 0;

    if (n_tgt != size_tgt || n_dft != size_dft) {
        SRV_ERR("disk cache: incomplete prefix state (tgt %zu/%zu, dft %zu/%zu)\n",
                n_tgt, size_tgt, n_dft, size_dft);
        return false;
    }

    return true;
}

void server_prompt_cache::mirror_latest(size_t budget) {
    if (!disk || !disk->active || states.empty()) {
        return;
    }

    // mirror the entry that was just added: the buffers are shared, not copied,
    // so the write is the only cost. dedup, the size limits and the queue budget
    // are all checked inside mirror()
    disk->mirror(states.back(), budget);
}

size_t server_prompt_cache::size() const {
    size_t res = 0;

    for (const auto & state : states) {
        res += state.size();
    }

    return res;
}

size_t server_prompt_cache::n_tokens() const {
    size_t res = 0;

    for (const auto & state : states) {
        res += state.prompt.n_tokens();
    }

    return res;
}

server_prompt_cache_state * server_prompt_cache::alloc(const server_prompt & prompt, size_t state_size_tgt, size_t state_size_dft) {
    // first check if the current state is contained fully in the cache
    for (auto it = states.begin(); it != states.end(); ++it) {
        const int cur_lcp_len = it->prompt.tokens.get_common_prefix(prompt.tokens);

        if (cur_lcp_len == (int) prompt.tokens.size()) {
            SRV_TRC("%s", " - prompt is already in the cache, skipping\n");
            return nullptr;
        }
    }

    // calculate checkpoints size to see if it will fit with the prompt
    size_t checkpoints_size = 0;
    for (const auto & ckpt : prompt.checkpoints) {
        checkpoints_size += ckpt->size();
    }

    const size_t state_size_new = state_size_tgt + state_size_dft + checkpoints_size;

    // skip over-limit entries to avoid disturbing the cache
    if (limit_size > 0 && state_size_new > limit_size) {
        SRV_WRN(" - prompt state size %.3f MiB exceeds cache size limit %.3f MiB, skipping\n",
                state_size_new / (1024.0 * 1024.0), limit_size / (1024.0 * 1024.0));
        return nullptr;
    }

    // remove any cached prompts that are fully contained in the current prompt
    for (auto it = states.begin(); it != states.end();) {
        const int len = it->prompt.tokens.get_common_prefix(prompt.tokens);

        if (len == (int) it->prompt.tokens.size()) {
            SRV_TRC(" - removing obsolete cached prompt with length %d\n", len);

            it = states.erase(it);
        } else {
            ++it;
        }
    }

    if (limit_size > 0) {
        // make room before allocating the new vectors to avoid breaching the limit
        while (!states.empty() && size() + state_size_new > limit_size) {
            SRV_WRN(" - making room for prompt cache entry, removing oldest entry (size = %.3f MiB)\n",
                    states.front().size() / (1024.0 * 1024.0));

            spill(states.front());
            states.pop_front();
        }
    }

    auto state_data_tgt = std::make_shared<common_state_data>();
    auto state_data_dft = std::make_shared<common_state_data>();

    // check if we can allocate enough memory for the new state
    try {
        state_data_tgt->resize(state_size_tgt);
        state_data_dft->resize(state_size_dft);
    } catch (const std::bad_alloc & e) {
        SRV_ERR("failed to allocate memory for prompt cache state: %s\n", e.what());

        limit_size = std::max<size_t>(1, 0.4*size());

        SRV_WRN(" - cache size limit reduced to %.3f MiB\n", limit_size / (1024.0 * 1024.0));

        update();

        return nullptr;
    }

    states.push_back({
        /*.prompt =*/ {
            /*.tokens      =*/ prompt.tokens.clone(),
            /*.checkpoints =*/ prompt.checkpoints,
        },
        /*.data   =*/ {
            /*.main =*/ std::move(state_data_tgt),
            /*.drft =*/ std::move(state_data_dft),
        },
    });

    return &states.back();
}

bool server_prompt_cache::load(server_prompt & prompt, const server_tokens & tokens_new, llama_context * ctx_tgt, llama_context * ctx_dft, int32_t id_slot,
        prompt_cache_restore_info & info) {
    int lcp_best = prompt.tokens.get_common_prefix(tokens_new);

    float f_keep_best = prompt.tokens.size() > 0 ? float(lcp_best) / prompt.tokens.size() : -1.0f; // empty slot: any cache entry wins
    float f_sim_best  = float(lcp_best) / tokens_new.size();

    SRV_TRC(" - looking for better prompt, base f_keep = %.3f, f_sim = %.3f\n", f_keep_best, f_sim_best);

    auto it_best = states.end();

    // find the most similar cached prompt, that would also preserve the most context
    for (auto it = states.begin(); it != states.end(); ++it) {
        const int lcp_cur = it->prompt.tokens.get_common_prefix(tokens_new);

        const float f_keep_cur = float(lcp_cur) / it->prompt.tokens.size();
        const float f_sim_cur  = float(lcp_cur) / tokens_new.size();

        SRV_TRC("   - prompt with length %7zu, lcp = %7d, f_keep = %.3f, f_sim = %.3f\n", it->prompt.tokens.size(), lcp_cur, f_keep_cur, f_sim_cur);

        // don't trash large prompts
        if (f_keep_cur < 0.25f) {
            continue;
        }

        if (f_keep_best < f_keep_cur && f_sim_best < f_sim_cur) {
            f_keep_best = f_keep_cur;
            f_sim_best  = f_sim_cur;
            lcp_best    = lcp_cur;

            it_best = it;
        }
    }

    if (it_best != states.end()) {
        SRV_TRC(" - found better prompt with f_keep = %.3f, f_sim = %.3f\n", f_keep_best, f_sim_best);

        const int64_t t_ram = ggml_time_us();

        {
            // release the buffer after the restore; if a pending disk mirror
            // still references it, the memory stays alive until the write
            // completes
            auto & data = it_best->data.main;

            const size_t size = data->size();
            const size_t n = llama_state_seq_set_data_ext(ctx_tgt, data->data(), size, id_slot, 0);
            if (n != size) {
                SRV_ERR("failed to restore state with size %zu\n", size);

                info.reject = cache_reject_reason::restore_failed;
                return false;
            }

            data.reset();
        }

        {
            auto & data = it_best->data.drft;

            if (data && !data->empty()) {
                GGML_ASSERT(ctx_dft);

                const size_t size = data->size();
                const size_t n = llama_state_seq_set_data_ext(ctx_dft, data->data(), size, id_slot, 0);
                if (n != size) {
                    SRV_WRN("failed to restore state with size %zu\n", size);

                    info.reject = cache_reject_reason::restore_failed;
                    return false;
                }

                data.reset();
            }
        }

        SRV_DBG("%s", "__TEST_TAG_PROMPT_CACHE_RESTORE__\n");

        // the whole cached prompt is a prefix of the new one
        if (lcp_best == (int) it_best->prompt.tokens.size()) {
            SRV_DBG("%s", "__TEST_TAG_PROMPT_CACHE_FULL_RESTORE__\n");
        }

        info.src           = prompt_cache_restore_info::source::ram;
        info.entry_tokens  = it_best->prompt.tokens.size();
        info.candidate_lcp = lcp_best;
        info.lcp           = lcp_best;
        info.elapsed_us    = ggml_time_us() - t_ram;

        prompt = std::move(it_best->prompt);

        states.erase(it_best);
    } else if (disk) {
        // nothing good enough in RAM - try the on-disk tier before prefilling again
        return disk->load(prompt, tokens_new, ctx_tgt, ctx_dft, id_slot, info);
    }

    return true;
}

void server_prompt_cache::update() {
    if (limit_size > 0) {
        while (!states.empty() && size() > limit_size) {
            SRV_WRN(" - cache size limit reached, removing oldest entry (size = %.3f MiB)\n", states.front().size() / (1024.0 * 1024.0));

            spill(states.front());
            states.pop_front();
        }
    }

    // average size per token
    const float size_per_token = std::max<float>(1.0f, float(size()) / (std::max<size_t>(1, n_tokens())));

    // dynamically increase the token limit if it can fit in the memory limit
    const size_t limit_tokens_cur = limit_size > 0 ? std::max<size_t>(limit_tokens, limit_size/size_per_token) : limit_tokens;

    if (limit_tokens > 0) {
        while (!states.empty() && n_tokens() > limit_tokens_cur) {
            SRV_WRN(" - cache token limit (%zu, est: %zu) reached, removing oldest entry (size = %.3f MiB)\n",
                    limit_tokens, limit_tokens_cur, states.front().size() / (1024.0 * 1024.0));

            spill(states.front());
            states.pop_front();
        }
    }

    SRV_TRC(" - cache state: %zu prompts, %.3f MiB (limits: %.3f MiB, %zu tokens, %zu est)\n",
            states.size(), size() / (1024.0 * 1024.0), limit_size / (1024.0 * 1024.0), limit_tokens, limit_tokens_cur);

    for (const auto & state : states) {
        SRV_TRC("   - prompt %p: %7d tokens, checkpoints: %2zu, %9.3f MiB\n",
                (const void *)&state, state.prompt.n_tokens(), state.prompt.checkpoints.size(), state.size() / (1024.0 * 1024.0));
    }
}
