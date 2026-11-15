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

    add_items("counter", counters);
    add_items("gauge",   gauges);

    // labeled counter: one time series per draft position
    if (!metrics.n_accepted_per_pos.empty()) {
        prometheus << "# HELP llamacpp:spec_decode_num_accepted_tokens_per_pos_total"
                      " Accepted tokens per draft position\n"
                   << "# TYPE llamacpp:spec_decode_num_accepted_tokens_per_pos_total counter\n";
        for (size_t i = 0; i < metrics.n_accepted_per_pos.size(); i++) {
            prometheus << "llamacpp:spec_decode_num_accepted_tokens_per_pos_total{position=\""
                       << i << "\"} " << metrics.n_accepted_per_pos[i] << "\n";
        }
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
//   uint32   packed[packed_size]  token ids plus media metadata
//   uint64   size_main
//   uint64   size_drft
//   uint32   n_ckpt
//   uint64   created       unix seconds
//   uint64   last_used     unix seconds (rewritten in place on every hit)
//   checkpoint table: n_ckpt x { int64 n_tokens, int32 pos_min, int32 pos_max, uint64 size_spec, uint64 size_tgt, uint64 size_dft }
//   main[size_main]
//   drft[size_drft]
//   checkpoint blobs, in table order: spec, tgt, dft
//

namespace {

constexpr char     DISK_CACHE_MAGIC[8] = { 'L', 'L', 'D', 'S', 'C', '0', '0', '1' };
constexpr uint32_t DISK_CACHE_VERSION  = 7;

// version 5 added the last_used field, version 6 added the crc_main/crc_drft
// integrity checksums, version 7 added per-checkpoint checksums. Header layout
// is fixed except for the model/context signature, so its size is
// DISK_CACHE_HEADER_FIXED + sig.size() bytes; touch_entry() rewrites the
// last_used field in place
constexpr size_t DISK_CACHE_HEADER_FIXED = 8 + 4 + 4 + 4 + 8 + 8 + 4 + 8 + 8 + 8 + 8; // magic..crc_drft, without sig (72)

// skip entries shorter than this in the lookup: the lcp with any prompt
// cannot reach min_tokens, so they can never be restored
constexpr size_t DISK_CACHE_MIN_LOOKUP_TOKENS = 64;

// cap the number of pending writes: the queued entries share the RAM cache
// buffers (shared_ptr, no extra memory), but a slow disk must not pin an
// unbounded number of them. the byte budget itself is configurable through
// --cache-disk-queue-max (common_params::cache_disk_queue_max_mib)
constexpr size_t DISK_CACHE_QUEUE_MAX_ENTRIES = 8;

// a restore always reads the whole entry file, so restoring a tiny shared
// prefix out of a huge entry costs far more disk time than the prefill it
// saves. the break-even point is roughly 1 MiB per restored token (a ~500 MB/s
// disk reads ~2 s/GB while prefill runs ~2 k tok/s); skip restores above this
// bytes-per-token cost
constexpr size_t DISK_CACHE_MAX_BYTES_PER_TOKEN = 768ull*1024ull;

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
// signature, version or any file size does not match
constexpr char     DISK_CACHE_INDEX_MAGIC[8] = { 'L', 'L', 'D', 'S', 'C', 'I', 'D', 'X' };
constexpr uint32_t DISK_CACHE_INDEX_VERSION  = 2; // v2 added size_main/size_drft (read_entry cross-checks them)
constexpr const char * DISK_CACHE_INDEX_NAME = "pc-index.dat";

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

size_t disk_cache_lcp(const llama_tokens & a, const llama_tokens & b) {
    const size_t n = std::min(a.size(), b.size());

    size_t i = 0;
    while (i < n && a[i] == b[i]) {
        ++i;
    }

    return i;
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

        // entries stored before prefix dedup was added may still hold a chain
        // of prefix prompts - drop the redundant ones
        dedup_prefixes();

        // trim an existing cache that no longer fits the current limit (e.g. after lowering --cache-disk-max)
        enforce_limit();

        // persist the startup index so the next boot can trust the metadata
        // without opening every entry file
        save_manifest();
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

void server_disk_cache::scan() {
    namespace fs = std::filesystem;

    std::error_code ec;

    // startup index written by a previous run: when a file's size matches its
    // record, the metadata is trusted and the entry file is not opened at all.
    // anything inconsistent (unknown file, size mismatch, corrupt record) falls
    // back to the full header validation below.
    std::map<std::string, disk_cache_manifest_meta> manifest;
    const bool manifest_ok = load_manifest(manifest);

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
                e.path      = de.path().string();
                e.hash      = mi->second.hash;
                e.size_file = (size_t) fsize;
                e.size_main = (size_t) mi->second.size_main;
                e.size_drft = (size_t) mi->second.size_drft;
                e.last_used = mi->second.last_used;

                // a corrupt index record is not trusted: fall through to the
                // full validation, which either repairs or drops the entry
                if (disk_cache_unpack_ids(mi->second.packed, e.tokens, e.media_indices, e.media_token_counts, e.media_ids) &&
                        e.hash == disk_cache_hash(e.tokens, e.media_indices, e.media_token_counts, e.media_ids)) {
                    total_size += e.size_file;
                    entries.push_back(std::move(e));
                    manifest.erase(mi);
                    continue;
                }
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
        };

        char magic[8];
        f.read(magic, sizeof(magic));

        uint32_t version = 0;
        uint32_t sig_len = 0;
        if (!disk_cache_read(f, version) || !disk_cache_read(f, sig_len) ||
                memcmp(magic, DISK_CACHE_MAGIC, sizeof(magic)) != 0 ||
                version != DISK_CACHE_VERSION) {
            drop("bad header");
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

        uint64_t size_main = 0;
        uint64_t size_drft = 0;
        uint32_t n_ckpt    = 0;
        uint64_t created   = 0;
        uint64_t last_used = 0;
        if (!disk_cache_read(f, size_main) || !disk_cache_read(f, size_drft) ||
                !disk_cache_read(f, n_ckpt) || !disk_cache_read(f, created) ||
                !disk_cache_read(f, last_used)) {
            drop("truncated header");
            continue;
        }

        e.hash      = disk_cache_hash(e.tokens, e.media_indices, e.media_token_counts, e.media_ids);
        e.size_main = (size_t) size_main;
        e.size_drft = (size_t) size_drft;
        e.last_used = (int64_t) (last_used > 0 ? last_used : created);
        e.size_file = (size_t) fsize; // stat'ed above, before the header was parsed

        total_size += e.size_file;
        entries.push_back(std::move(e));
    }
}

bool server_disk_cache::load_manifest(std::map<std::string, disk_cache_manifest_meta> & out) {
    std::ifstream f(dir + "/" + DISK_CACHE_INDEX_NAME, std::ios::binary);
    if (!f) {
        return false;
    }

    char magic[8];
    f.read(magic, sizeof(magic));

    uint32_t version = 0;
    uint32_t sig_len = 0;
    if (!f || memcmp(magic, DISK_CACHE_INDEX_MAGIC, sizeof(magic)) != 0 ||
            !disk_cache_read(f, version) || version != DISK_CACHE_INDEX_VERSION ||
            !disk_cache_read(f, sig_len) || sig_len != sig.size()) {
        return false;
    }

    std::string entry_sig(sig_len, '\0');
    f.read(entry_sig.data(), (std::streamsize) sig_len);
    if (!f || entry_sig != sig) {
        return false;
    }

    uint32_t n_records = 0;
    if (!disk_cache_read(f, n_records) || n_records > (1u << 20)) {
        return false;
    }

    for (uint32_t i = 0; i < n_records; ++i) {
        uint32_t name_len = 0;
        if (!disk_cache_read(f, name_len) || name_len == 0 || name_len > 256) {
            return false;
        }

        disk_cache_manifest_meta m;

        std::string name(name_len, '\0');
        f.read(name.data(), (std::streamsize) name_len);
        uint64_t packed_size = 0;
        if (!f ||
                !disk_cache_read(f, m.hash) || !disk_cache_read(f, m.size_file) ||
                !disk_cache_read(f, m.size_main) || !disk_cache_read(f, m.size_drft) ||
                !disk_cache_read(f, m.last_used) ||
                !disk_cache_read(f, packed_size) || packed_size > (1u << 28)) {
            return false;
        }

        m.packed.resize((size_t) packed_size);
        if (packed_size > 0) {
            f.read(m.packed.data(), (std::streamsize) m.packed.size());
        }
        if (!f) {
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

        rec.hash      = e.hash;
        rec.size_file = (uint64_t) e.size_file;
        rec.size_main = (uint64_t) e.size_main;
        rec.size_drft = (uint64_t) e.size_drft;
        rec.last_used = e.last_used;

        records.push_back(std::move(rec));
    }

    const std::string path = dir + "/" + DISK_CACHE_INDEX_NAME;
    const std::string tmp  = path + ".tmp";

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
        disk_cache_write(f, (uint32_t) records.size());

        for (const auto & rec : records) {
            disk_cache_write(f, (uint32_t) rec.name.size());
            f.write(rec.name.data(), (std::streamsize) rec.name.size());
            disk_cache_write(f, rec.hash);
            disk_cache_write(f, rec.size_file);
            disk_cache_write(f, rec.size_main);
            disk_cache_write(f, rec.size_drft);
            disk_cache_write(f, (uint64_t) rec.last_used);
            disk_cache_write(f, (uint64_t) rec.packed.size());
            f.write(rec.packed.data(), (std::streamsize) rec.packed.size());
        }

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

bool server_disk_cache::write_entry(const server_disk_cache_pending & pending, std::string & path_out, size_t & size_out) {
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
        // stored in the header, so read_entry can verify the bytes it restores
        const uint64_t crc_main = disk_cache_crc64(*pending.main);
        const uint64_t crc_drft = disk_cache_crc64(*pending.drft);

        disk_cache_write(f, (uint64_t) pending.main->size());
        disk_cache_write(f, (uint64_t) pending.drft->size());
        disk_cache_write(f, (uint32_t) ckpts.size());
        disk_cache_write(f, (uint64_t) disk_cache_now());
        disk_cache_write(f, (uint64_t) disk_cache_now()); // last_used
        disk_cache_write(f, crc_main);
        disk_cache_write(f, crc_drft);

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

    std::error_code ec;
    fs::remove(path, ec); // the name is content-addressed, a stale file may exist after a crash
    fs::rename(tmp, path, ec);
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

bool server_disk_cache::read_entry(const server_disk_cache_entry & e, size_t lcp, std::vector<char> & packed, common_state_data & main, common_state_data & drft,
        std::list<std::shared_ptr<const common_prompt_checkpoint>> & checkpoints) {
    std::ifstream f(e.path, std::ios::binary);
    if (!f) {
        return false;
    }

    char magic[8];
    f.read(magic, sizeof(magic));

    uint32_t version = 0;
    uint32_t sig_len = 0;
    if (!disk_cache_read(f, version) || !disk_cache_read(f, sig_len) ||
            memcmp(magic, DISK_CACHE_MAGIC, sizeof(magic)) != 0 ||
            version != DISK_CACHE_VERSION || sig_len != sig.size()) {
        return false;
    }

    std::string entry_sig(sig_len, '\0');
    f.read(entry_sig.data(), (std::streamsize) sig_len);
    if (entry_sig != sig) {
        return false;
    }

    uint32_t packed_size = 0;
    if (!disk_cache_read(f, packed_size) || packed_size > (1u << 28)) {
        return false;
    }

    packed.resize(packed_size);
    f.read(packed.data(), (std::streamsize) packed.size());
    if (!f) {
        return false;
    }

    uint64_t size_main = 0;
    uint64_t size_drft = 0;
    uint32_t n_ckpt    = 0;
    uint64_t created   = 0;
    uint64_t last_used = 0;
    uint64_t crc_main  = 0;
    uint64_t crc_drft  = 0;
    if (!disk_cache_read(f, size_main) || !disk_cache_read(f, size_drft) ||
            !disk_cache_read(f, n_ckpt) || !disk_cache_read(f, created) ||
            !disk_cache_read(f, last_used) || !disk_cache_read(f, crc_main) ||
            !disk_cache_read(f, crc_drft)) {
        return false;
    }

    if (size_main != e.size_main || size_drft != e.size_drft || n_ckpt > 1024) {
        return false;
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
            return false;
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

    main.resize(size_main);
    if (size_main > 0) {
        f.read(reinterpret_cast<char *>(main.data()), (std::streamsize) size_main);
    }

    drft.resize(size_drft);
    if (size_drft > 0) {
        f.read(reinterpret_cast<char *>(drft.data()), (std::streamsize) size_drft);
    }

    if (!f) {
        return false;
    }

    // verify the payload before it reaches llama_state_seq_set_data_ext: a torn
    // write or a bad disk sector would otherwise be restored silently and produce
    // wrong inference. a mismatch marks the entry for removal by the caller.
    if (disk_cache_crc64(main) != crc_main || disk_cache_crc64(drft) != crc_drft) {
        SRV_WRN("disk cache: payload checksum mismatch in '%s', dropping it\n", e.path.c_str());
        return false;
    }

    checkpoints.clear();
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
            checkpoints.clear();
            return false;
        }

        // the checkpoint payload feeds llama_state_seq_set_data_ext as well, so
        // it gets the same integrity check as the main/draft states
        if (disk_cache_crc64(ckpt->data_spec) != m.crc_spec ||
                disk_cache_crc64(ckpt->data_tgt) != m.crc_tgt ||
                disk_cache_crc64(ckpt->data_dft) != m.crc_dft) {
            SRV_WRN("disk cache: checkpoint checksum mismatch in '%s', dropping it\n", e.path.c_str());
            checkpoints.clear();
            return false;
        }

        checkpoints.push_back(ckpt);
    }

    const uint64_t bytes_read = (uint64_t) hdr_end + size_main + size_drft + ckpt_bytes;
    SRV_TRC("disk cache: read %.2f MiB of %.2f MiB, checkpoints %d/%zu\n",
            bytes_read/1048576.0, e.size_file/1048576.0, want >= 0 ? 1 : 0, metas.size());

    return true;
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

    // last_used is the 8 bytes right after size_main/size_drft/n_ckpt/created,
    // which follow the variable-length signature and packed metadata
    const std::streamoff off_last_used = (std::streamoff) (DISK_CACHE_HEADER_FIXED - 8 + sig_len + packed_size);

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

    std::error_code ec;
    std::filesystem::remove(path, ec);

    // only reclaim the quota when the file is actually gone: on Windows the
    // most common remove failure is a sharing violation (Defender scanning,
    // unreleased handle) - the file still occupies its gigabytes, so counting
    // it as freed would let enforce_limit underestimate usage and the
    // directory could silently exceed --cache-disk-max until the next restart
    // scan. the orphan file is picked up by the startup scan and dropped as a
    // bad-header entry. the index entry is removed either way, so a poisoned
    // entry cannot loop
    const bool removed = !ec || ec == std::errc::no_such_file_or_directory;

    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->path == path) {
            if (removed) {
                total_size -= std::min(total_size, it->size_file);
            } else {
                SRV_WRN("disk cache: could not remove '%s' (%s), not reclaiming %.2f MiB\n",
                        path.c_str(), ec.message().c_str(), it->size_file/(1024.0*1024.0));
            }
            entries.erase(it);
            break;
        }
    }

    // the entry set changed - persist the startup index
    save_manifest();
}

bool server_disk_cache::covered_cheaply(const llama_tokens & ids) const {
    // mtx must be held
    for (const auto & e : entries) {
        // media placeholders are only comparable through the packed metadata, so
        // an entry that carries images never stands in for another prompt
        if (!e.media_indices.empty()) {
            continue;
        }
        if (e.tokens.size() + DISK_CACHE_MAX_PREFIX_LAG_TOKENS < ids.size()) {
            continue;
        }
        // an exact token prefix is not required: appending text can re-merge the
        // tokens around the cut, and a restore truncates at the divergence point
        // anyway, so what matters is that all but a short tail is already shared
        if (disk_cache_lcp(e.tokens, ids) + DISK_CACHE_MAX_PREFIX_LAG_TOKENS >= ids.size()) {
            return true;
        }
    }
    return false;
}

void server_disk_cache::drop_prefixes(const llama_tokens & ids, const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids) {
    namespace fs = std::filesystem;

    // mtx must be held
    for (auto it = entries.begin(); it != entries.end(); ) {
        const llama_tokens & old_ids = it->tokens;
        if (old_ids.size() < ids.size() && disk_cache_lcp(old_ids, ids) == old_ids.size()) {
            // the token sequence is a strict prefix, but the media placeholders
            // could differ: every media chunk of the shorter entry must also be
            // present in the longer one at the same position with the same id
            bool media_ok = true;
            for (size_t i = 0; i < it->media_indices.size() && media_ok; ++i) {
                const uint32_t idx = it->media_indices[i];
                const auto m = std::lower_bound(media_indices.begin(), media_indices.end(), idx);
                if (m == media_indices.end() || *m != idx) {
                    media_ok = false;
                    break;
                }
                const size_t j = (size_t) (m - media_indices.begin());
                if (it->media_token_counts[i] != media_token_counts[j] || it->media_ids[i] != media_ids[j]) {
                    media_ok = false;
                }
            }

            if (media_ok) {
                const size_t size_file = std::min(total_size, it->size_file);
                std::error_code ec;
                fs::remove(it->path, ec);
                SRV_INF("disk cache: dropping '%s': prefix of a stored prompt, %zu tokens, %.2f MiB\n",
                        it->path.c_str(), old_ids.size(), size_file/(1024.0*1024.0));
                total_size -= size_file;
                it = entries.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }
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

            // a is a strict prefix of b (and b is not a prefix of a)
            const bool a_in_b = a.tokens.size() < b.tokens.size() && disk_cache_lcp(a.tokens, b.tokens) == a.tokens.size();
            const bool b_in_a = b.tokens.size() < a.tokens.size() && disk_cache_lcp(b.tokens, a.tokens) == b.tokens.size();
            if (!a_in_b && !b_in_a) {
                continue;
            }

            // the shorter one is the redundant prefix; it must also have the
            // media chunks of the longer one as a subset to be safely droppable
            const auto & shorter = a_in_b ? a : b;
            const auto & longer  = a_in_b ? b : a;

            bool media_ok = true;
            for (size_t k = 0; k < shorter.media_indices.size() && media_ok; ++k) {
                const uint32_t idx = shorter.media_indices[k];
                const auto m = std::lower_bound(longer.media_indices.begin(), longer.media_indices.end(), idx);
                if (m == longer.media_indices.end() || *m != idx) {
                    media_ok = false;
                    break;
                }
                const size_t p = (size_t) (m - longer.media_indices.begin());
                if (shorter.media_token_counts[k] != longer.media_token_counts[p] || shorter.media_ids[k] != longer.media_ids[p]) {
                    media_ok = false;
                }
            }
            if (!media_ok) {
                continue;
            }

            const size_t shorter_idx = a_in_b ? i : j;
            if (!drop[shorter_idx]) {
                std::error_code ec;
                fs::remove(entries[shorter_idx].path, ec);
                SRV_INF("disk cache: dropping '%s': prefix of a stored prompt, %zu tokens, %.2f MiB\n",
                        entries[shorter_idx].path.c_str(), entries[shorter_idx].tokens.size(),
                        entries[shorter_idx].size_file/(1024.0*1024.0));
                total_size -= std::min(total_size, entries[shorter_idx].size_file);
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

bool server_disk_cache::load(server_prompt & prompt, const server_tokens & tokens_new, llama_context * ctx_tgt, llama_context * ctx_dft, int32_t id_slot) {
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

    server_disk_cache_entry best;
    bool found = false;

    size_t best_lcp = 0;

    {
        std::lock_guard<std::mutex> lock(mtx);

        SRV_TRC("disk cache: lookup for %zu tokens, %zu entries, min %zu tokens\n", tokens.size(), entries.size(), min_tokens);

        for (const auto & e : entries) {
            const size_t e_size = e.tokens.size();

            // an entry shorter than the minimum can never yield a restorable lcp
            if (e_size < DISK_CACHE_MIN_LOOKUP_TOKENS || e_size < min_tokens) {
                continue;
            }

            // the lcp cannot exceed either length; if that is below the minimum,
            // skip the whole-token comparison
            if (std::min(e_size, tokens.size()) < min_tokens) {
                continue;
            }

            size_t lcp = disk_cache_lcp(e.tokens, tokens);
            if (e.media_indices.size() != e.media_token_counts.size() || e.media_indices.size() != e.media_ids.size()) {
                continue;
            }
            for (size_t i = 0; i < e.media_indices.size(); ++i) {
                const uint32_t idx = e.media_indices[i];
                if ((size_t) idx >= lcp) {
                    continue;
                }
                const auto it = std::lower_bound(media_indices.begin(), media_indices.end(), idx);
                if (it == media_indices.end() || *it != idx) {
                    lcp = std::min(lcp, (size_t) idx);
                    break;
                }
                const size_t j = (size_t) (it - media_indices.begin());
                if (e.media_token_counts[i] != media_token_counts[j] || e.media_ids[i] != media_ids[j]) {
                    lcp = std::min(lcp, (size_t) idx);
                    break;
                }
            }
            for (size_t i = 0; i < media_indices.size(); ++i) {
                const uint32_t idx = media_indices[i];
                const auto it = std::lower_bound(e.media_indices.begin(), e.media_indices.end(), idx);
                if (it == e.media_indices.end() || *it != idx) {
                    lcp = std::min(lcp, (size_t) idx);
                    break;
                }
                const size_t j = (size_t) (it - e.media_indices.begin());
                if (media_token_counts[i] != e.media_token_counts[j] || media_ids[i] != e.media_ids[j]) {
                    lcp = std::min(lcp, (size_t) idx);
                    break;
                }
            }
            for (size_t i = 0; i < e.media_indices.size(); ++i) {
                const size_t start = e.media_indices[i];
                const size_t end = start + e.media_token_counts[i];
                if (lcp > start && lcp < end) {
                    lcp = start;
                    break;
                }
            }

            SRV_TRC("disk cache:   - entry with length %7zu, lcp = %7zu, f_keep = %.3f\n",
                    e_size, lcp, float(lcp) / e_size);

            // unlike the RAM cache there is no eviction cost here: a restore
            // reads the file and keeps every entry, so the f_keep floor does
            // not apply. min_tokens is the only match-quality guard - it keeps
            // the file read cheaper than the prefill it replaces.
            if (lcp < min_tokens) {
                continue;
            }

            // a restore always reads the whole entry file, so skip entries
            // where the bytes read per restored token clearly cost more disk
            // time than the prefill of the shared prefix would take
            if (e.size_file > lcp * DISK_CACHE_MAX_BYTES_PER_TOKEN) {
                SRV_TRC("disk cache:   - skipping entry with length %7zu: lcp %7zu would read %.2f MiB per token\n",
                        e_size, lcp, e.size_file/(1024.0*1024.0)/std::max<size_t>(1, lcp));
                continue;
            }

            // the longest shared prefix wins; on a tie the most recently used
            // entry is fresher and the more similar one keeps more of the cache
            if (!found || lcp > best_lcp || (lcp == best_lcp && e.last_used > best.last_used)) {
                best_lcp = lcp;
                best     = e;
                found    = true;
            }
        }
    }

    if (!found) {
        return true;
    }

    const float f_keep = float(best_lcp) / best.tokens.size();
    const float f_sim  = float(best_lcp) / std::max<size_t>(1, tokens.size());

    SRV_TRC("disk cache: candidate '%s' with %zu tokens, f_keep = %.3f, f_sim = %.3f\n",
            best.path.c_str(), best.tokens.size(), f_keep, f_sim);

    const int64_t t_start = ggml_time_us();

    common_state_data main;
    common_state_data drft;
    std::vector<char> packed;
    std::list<std::shared_ptr<const common_prompt_checkpoint>> checkpoints;
    if (!read_entry(best, best_lcp, packed, main, drft, checkpoints)) {
        SRV_WRN("disk cache: failed to read '%s', dropping it\n", best.path.c_str());
        drop_entry(best.path);

        return true;
    }

    // from here on the KV cells are being replaced with the entry's data while
    // slot.prompt still holds the previous tokens: any failure must return
    // false so the caller clears the slot and prefills from scratch. returning
    // true would leave KV and prompt.tokens mismatched -> silent wrong inference
    const size_t size_main = main.size();
    if (llama_state_seq_set_data_ext(ctx_tgt, main.data(), size_main, id_slot, 0) != size_main) {
        // keep the entry: the failure can be transient (e.g. VRAM pressure),
        // and a retry on the next request may succeed
        SRV_WRN("disk cache: failed to restore the target state (size %zu)\n", size_main);
        return false;
    }

    const size_t size_drft = drft.size();
    if (size_drft > 0) {
        if (!ctx_dft) {
            // deterministic: this process has no draft context, the entry can
            // never be restored here - drop it instead of failing on every
            // matching request
            SRV_WRN("%s", "disk cache: entry has a draft state but there is no draft context, dropping it\n");
            drop_entry(best.path);
            return false;
        }
        if (llama_state_seq_set_data_ext(ctx_dft, drft.data(), size_drft, id_slot, 0) != size_drft) {
            SRV_WRN("disk cache: failed to restore the draft state (size %zu)\n", size_drft);
            return false;
        }
    }

    llama_tokens restored_tokens;
    std::vector<uint32_t> restored_media_indices;
    std::vector<uint32_t> restored_media_token_counts;
    std::vector<std::string> restored_media_ids;
    if (!disk_cache_unpack_ids(packed, restored_tokens, restored_media_indices, restored_media_token_counts, restored_media_ids)) {
        // deterministic corruption of the packed metadata - the entry can
        // never be restored, drop it instead of failing on every match
        SRV_WRN("disk cache: failed to decode the cached prompt metadata of '%s', dropping it\n", best.path.c_str());
        drop_entry(best.path);
        return false;
    }

    try {
        for (size_t i = 0; i < restored_media_indices.size(); ++i) {
            const uint32_t idx = restored_media_indices[i];
            if (idx >= best_lcp || idx >= tokens_new.size() ||
                    tokens_new.media_chunk_token_count(idx) != restored_media_token_counts[i] ||
                    tokens_new.media_chunk_id(idx) != restored_media_ids[i]) {
                throw std::runtime_error("cached media metadata differs from the current prompt");
            }
        }

        // only the shared prefix (best_lcp tokens) was restored from disk: the
        // remainder of the cached prompt diverges from the current one and must
        // not be kept, otherwise the media-placeholder fill would compare tokens
        // past the prefix and reject the restore
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
        // deterministic media mismatch - same as above, drop the entry
        SRV_WRN("disk cache: failed to restore prompt metadata of '%s': %s, dropping it\n", best.path.c_str(), err.what());
        drop_entry(best.path);
        return false;
    }

    prompt.checkpoints = std::move(checkpoints);

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
    SRV_DBG("disk cache: restored %zu/%zu tokens (%zu + %zu bytes) in %.2f ms\n",
            best_lcp, best.tokens.size(), size_main, size_drft, (ggml_time_us() - t_start)/1000.0);

    return true;
}

void server_disk_cache::store(server_prompt_cache_state && state) {
    if (!active) {
        return;
    }

    if (state.prompt.tokens.size() < min_tokens) {
        return;
    }

    server_disk_cache_pending pending;

    if (!disk_cache_pack(state.prompt.tokens, pending.packed, pending.ids,
                pending.media_indices, pending.media_token_counts, pending.media_ids)) {
        SRV_TRC("%s", "disk cache: prompt metadata cannot be packed, skipping\n");
        return;
    }

    if (pending.ids.size() < min_tokens) {
        return;
    }

    // checkpoints travel with the entry: hybrid/recurrent models need them to reuse a long prefix
    pending.bytes = state.size();

    if (limit_size > 0 && pending.bytes > limit_size) {
        SRV_WRN("disk cache: entry of %.2f MiB exceeds the %.2f MiB limit, skipping\n",
                pending.bytes/(1024.0*1024.0), limit_size/(1024.0*1024.0));
        return;
    }

    const uint64_t hash = disk_cache_hash(pending.ids, pending.media_indices, pending.media_token_counts, pending.media_ids);

    {
        std::lock_guard<std::mutex> lock(mtx);

        for (const auto & e : entries) {
            if (e.hash == hash && e.tokens.size() == pending.ids.size()) {
                SRV_TRC("disk cache: %016" PRIx64 " is already stored, skipping\n", hash);
                return;
            }
        }

        if (inflight.count(hash)) {
            SRV_TRC("disk cache: %016" PRIx64 " is already queued, skipping\n", hash);
            return;
        }

        if (covered_cheaply(pending.ids)) {
            SRV_INF("disk cache: %zu tokens are covered up to a %zu-token tail by a stored entry, skipping a %.2f MiB write\n",
                    pending.ids.size(), DISK_CACHE_MAX_PREFIX_LAG_TOKENS, pending.bytes/(1024.0*1024.0));
            return;
        }

        if ((queue_bytes_max > 0 && queue_bytes + pending.bytes > queue_bytes_max) ||
                queue.size() >= DISK_CACHE_QUEUE_MAX_ENTRIES) {
            SRV_WRN("disk cache: write queue is full (%zu entries, %.2f MiB pending), skipping a prompt with %zu tokens\n",
                    queue.size(), queue_bytes/(1024.0*1024.0), pending.ids.size());
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
    }

    cv.notify_one();
}

bool server_disk_cache::mirror(const server_prompt_cache_state & state, size_t budget_left) {
    if (!active) {
        return false;
    }

    if (state.prompt.tokens.size() < min_tokens) {
        return false;
    }

    const size_t bytes = state.size();

    if (limit_size > 0 && bytes > limit_size) {
        return false;
    }

    if (budget_left < bytes) {
        return false;
    }

    server_disk_cache_pending pending;

    if (!disk_cache_pack(state.prompt.tokens, pending.packed, pending.ids,
                pending.media_indices, pending.media_token_counts, pending.media_ids)) {
        return false;
    }

    if (pending.ids.size() < min_tokens) {
        return false;
    }

    pending.bytes = bytes;

    const uint64_t hash = disk_cache_hash(pending.ids, pending.media_indices, pending.media_token_counts, pending.media_ids);

    {
        std::lock_guard<std::mutex> lock(mtx);

        // already written, or already waiting in the queue
        for (const auto & e : entries) {
            if (e.hash == hash && e.tokens.size() == pending.ids.size()) {
                return false;
            }
        }
        if (inflight.count(hash)) {
            return false;
        }

        if (covered_cheaply(pending.ids)) {
            SRV_INF("disk cache: %zu tokens are covered up to a %zu-token tail by a stored entry, skipping a %.2f MiB mirror\n",
                    pending.ids.size(), DISK_CACHE_MAX_PREFIX_LAG_TOKENS, pending.bytes/(1024.0*1024.0));
            return false;
        }

        if ((queue_bytes_max > 0 && queue_bytes + pending.bytes > queue_bytes_max) ||
                queue.size() >= DISK_CACHE_QUEUE_MAX_ENTRIES) {
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
    }

    cv.notify_one();

    return true;
}

void server_disk_cache::sweep_expired() {
    namespace fs = std::filesystem;

    // mtx must be held
    if (ttl_seconds <= 0) {
        return;
    }

    const int64_t now = disk_cache_now();

    for (auto it = entries.begin(); it != entries.end(); ) {
        if (now - it->last_used > ttl_seconds) {
            SRV_INF("disk cache: dropping '%s': expired after %lld s idle (%.2f MiB)\n",
                    it->path.c_str(), (long long) (now - it->last_used), it->size_file/(1024.0*1024.0));

            std::error_code ec;
            fs::remove(it->path, ec);

            total_size -= std::min(total_size, it->size_file);
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
}

void server_disk_cache::enforce_limit() {
    namespace fs = std::filesystem;

    // mtx must be held
    sweep_expired();

    while (limit_size > 0 && total_size > limit_size && !entries.empty()) {
        auto it = std::min_element(entries.begin(), entries.end(),
                [](const server_disk_cache_entry & a, const server_disk_cache_entry & b) {
                    return a.last_used < b.last_used;
                });

        std::error_code ec;
        fs::remove(it->path, ec);

        SRV_WRN("disk cache: limit reached, removing '%s' (%.2f MiB)\n",
                it->path.c_str(), it->size_file/(1024.0*1024.0));

        total_size -= std::min(total_size, it->size_file);
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

        // skip the write when a longer stored entry already has this prompt as
        // a strict prefix: the longer one serves every lookup this one would,
        // so writing it would only waste disk
        bool redundant = false;
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (const auto & e : entries) {
                if (pending.ids.size() < e.tokens.size() &&
                        disk_cache_lcp(pending.ids, e.tokens) == pending.ids.size()) {
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

        const bool ok = write_entry(pending, path, size_file);

        {
            std::lock_guard<std::mutex> lock(mtx);

            queue_bytes -= std::min(queue_bytes, bytes);
            inflight.erase(hash);

            if (ok) {
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

                total_size += size_file;
                entries.push_back(std::move(e));

                // this prompt subsumes any shorter stored prompt that is its
                // prefix, so those files are dead weight - drop them
                drop_prefixes(pending.ids, pending.media_indices, pending.media_token_counts, pending.media_ids);

                enforce_limit();

                // the entry set changed - persist the startup index
                save_manifest();
            }
        }

        if (ok) {
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

bool server_prompt_cache::load(server_prompt & prompt, const server_tokens & tokens_new, llama_context * ctx_tgt, llama_context * ctx_dft, int32_t id_slot) {
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

        {
            // release the buffer after the restore; if a pending disk mirror
            // still references it, the memory stays alive until the write
            // completes
            auto & data = it_best->data.main;

            const size_t size = data->size();
            const size_t n = llama_state_seq_set_data_ext(ctx_tgt, data->data(), size, id_slot, 0);
            if (n != size) {
                SRV_ERR("failed to restore state with size %zu\n", size);

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

        prompt = std::move(it_best->prompt);

        states.erase(it_best);
    } else if (disk) {
        // nothing good enough in RAM - try the on-disk tier before prefilling again
        return disk->load(prompt, tokens_new, ctx_tgt, ctx_dft, id_slot);
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
