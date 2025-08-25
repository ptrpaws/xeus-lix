#include "lix_interpreter.hpp"
#include "lix_logger.hpp"

#include <algorithm>
#include <memory>
#include <regex>
#include <string_view>

#include "lix/config.h"
#include "lix/libcmd/markdown.hh"
#include "lix/libexpr/eval-error.hh"
#include "lix/libexpr/eval.hh"
#include "lix/libexpr/flake/flake.hh"
#include "lix/libstore/store-api.hh"
#include "lix/libutil/canon-path.hh"
#include "lix/libutil/error.hh"
#include "lix/libutil/signals.hh"
#include "lix/libutil/strings.hh"

#include "xeus-zmq/xserver_zmq.hpp"
#include "xeus/xhelper.hpp"

namespace xeus_lix
{
    interpreter::interpreter()
        : m_aio(std::make_unique<nix::AsyncIoRoot>())
        , m_store(m_aio->blockOn(nix::openStore()))
        , m_evaluator(std::make_unique<nix::Evaluator>(*m_aio, nix::SearchPath{}, m_store))
        , m_evalState(m_evaluator->begin(*m_aio))
        , m_localEnv(nullptr)
        , m_staticEnv(nullptr)
        , m_displacement(0)
        , m_logger(std::make_unique<JupyterLogger>(this))
    {
        initialize_scope();
        // redirect Lix's global logger to our Jupyter logger
        nix::logger = m_logger.get();
        nix::verbosity = nix::lvlInfo;
    }

    interpreter::~interpreter()
    {
        // restore default Lix logger
        nix::logger = nix::makeSimpleLogger();
    }

    void interpreter::initialize_scope()
    {
        m_staticEnv = std::make_shared<nix::StaticEnv>(nullptr, m_evaluator->builtins.staticEnv.get());
        m_localEnv = &m_evaluator->mem.allocEnv(NIX_ENV_SIZE);
        m_localEnv->up = &m_evaluator->builtins.env;
        m_displacement = 0;
    }

    void interpreter::configure_impl(){}

    void interpreter::shutdown_request_impl(){}

    void interpreter::execute_chunk(const std::string& chunk, bool is_last_chunk, int execution_counter)
    {
        std::string first_meaningful_line;
        bool has_meaningful_content = false;
        {
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line))
            {
                std::string trimmed_line = nix::trim(line);
                if (!trimmed_line.empty() && trimmed_line[0] != '#')
                {
                    first_meaningful_line = trimmed_line;
                    has_meaningful_content = true;
                    break;
                }
            }
        }

        if (!has_meaningful_content)
        {
            return;
        }

        if (first_meaningful_line[0] == '!')
        {
            execute_shell_command(chunk);
        }
        else if (first_meaningful_line[0] == ':')
        {
            handle_repl_command(nix::trim(chunk));
        }
        else
        {
            auto result_variant = m_evaluator->parseReplInput(chunk, nix::CanonPath::fromCwd(), m_staticEnv);
            std::visit(
                nix::overloaded{
                    [&](nix::ExprReplBindings& bindings) {
                        for (auto& [name, expr] : bindings.symbols)
                        {
                            nix::Value* val = m_evaluator->mem.allocValue();
                            expr->eval(*m_evalState, *m_localEnv, *val);
                            (void)expr.release();
                            m_staticEnv->vars.insert_or_assign(name, m_displacement);
                            m_localEnv->values[m_displacement++] = val;
                        }
                    },
                    [&](std::unique_ptr<nix::Expr>& expr) {
                        nix::Value val(nix::Value::null_t{});
                        expr->eval(*m_evalState, *m_localEnv, val);
                        (void)expr.release();

                        nl::json pub_data;
                        bool is_publishable = false;

                        // check for rich MIME type representations
                        try
                        {
                            m_evalState->forceValue(val, nix::noPos);
                            if (val.type() == nix::nAttrs)
                            {
                                // a `_toMime` attribute signals a rich representation
                                if (auto it = val.attrs->find(m_evaluator->symbols.create("_toMime"));
                                    it != val.attrs->end())
                                {
                                    nix::Value& mime_set_val = *it->value;
                                    m_evalState->forceAttrs(mime_set_val, it->pos, "while rendering _toMime");

                                    nl::json data_bundle;
                                    for (const auto& attr : *mime_set_val.attrs)
                                    {
                                        std::string_view mime_type_sv = m_evaluator->symbols[attr.name];
                                        std::string mime_type(mime_type_sv);
                                        nix::Value& data_val_ref = *attr.value;
                                        std::string data_str(
                                            m_evalState->forceString(data_val_ref, attr.pos, "mime data")
                                        );
                                        // TODO: handle escape sequences or verify the frontend does so
                                        data_bundle[mime_type] = data_str;
                                    }
                                    pub_data["data"] = data_bundle;
                                    pub_data["metadata"] = nl::json::object();
                                    is_publishable = true;
                                }
                            }
                        }
                        catch (...)
                        {
                            // fallback to 'text/plain' if _toMime lookup fails
                        }

                        if (is_publishable)
                        {
                            if (is_last_chunk)
                            {
                                publish_execution_result(
                                    execution_counter,
                                    std::move(pub_data["data"]),
                                    std::move(pub_data["metadata"])
                                );
                            }
                            else
                            {
                                if (pub_data["data"].contains("text/plain"))
                                {
                                    publish_stream("stdout", pub_data["data"]["text/plain"].get<std::string>() + "\n");
                                }
                            }
                        }
                        else
                        {
                            // fallback to 'text/plain'
                            std::stringstream ss;
                            nix::printValue(
                                *m_evalState,
                                ss,
                                val,
                                nix::PrintOptions{ .ansiColors = true, .force = true, .prettyIndent = 2 }
                            );
                            if (is_last_chunk)
                            {
                                nl::json res;
                                res["text/plain"] = ss.str();
                                publish_execution_result(execution_counter, std::move(res), nl::json::object());
                            }
                            else
                            {
                                publish_stream("stdout", ss.str() + "\n");
                            }
                        }
                    } },
                result_variant
            );
        }
    }

    void interpreter::execute_request_impl(
        send_reply_callback cb,
        int execution_counter,
        const std::string& code,
        xeus::execute_request_config,
        nl::json
    )
    {
        // helper to publish errors to the frontend.
        auto send_error = [&](const std::string& ename, const std::string& evalue) {
            std::vector<std::string> traceback = { evalue };
            publish_execution_error(ename, evalue, traceback);
            cb(xeus::create_error_reply(evalue, ename, traceback));
        };

        try
        {
            nix::unsetUserInterruptRequest();

            // code cells can contain multiple expressions, REPL commands, and shell commands
            // this logic splits the code into executable chunks
            std::vector<std::string> chunks;
            std::string current_buffer;
            std::istringstream code_stream(code);
            std::string line;
            bool in_shell_block = false;

            auto flush_buffer = [&]() {
                if (!nix::trim(current_buffer).empty())
                {
                    chunks.push_back(current_buffer);
                }
                current_buffer.clear();
            };

            while (std::getline(code_stream, line))
            {
                std::string trimmed_line = nix::trim(line);
                bool is_shell_line_start = !trimmed_line.empty() && trimmed_line.front() == '!';

                if (in_shell_block)
                {
                    current_buffer += line + '\n';
                    if (trimmed_line.empty() || trimmed_line.back() != '\\')
                    {
                        in_shell_block = false;
                        flush_buffer();
                    }
                }
                else if (is_shell_line_start)
                {
                    flush_buffer();
                    current_buffer += line + '\n';
                    if (!trimmed_line.empty() && trimmed_line.back() == '\\')
                    {
                        in_shell_block = true;
                    }
                    else
                    {
                        flush_buffer();
                    }
                }
                else if (!trimmed_line.empty() && trimmed_line.front() == ':')
                {
                    flush_buffer();
                    chunks.push_back(line + '\n');
                }
                else
                {
                    if (current_buffer.empty() && trimmed_line.empty())
                    {
                        continue;
                    }

                    current_buffer += line + '\n';

                    // heuristic to split expressions:
                    // try to parse the current buffer
                    // if it parses successfully, it's a complete chunk
                    // if it fails with an "unexpected end of file" error, it's incomplete
                    // if it fails with another parse error, assume a syntax error and treat
                    // the buffer as a chunk to let the evaluator report the error
                    try
                    {
                        (void)m_evaluator->parseReplInput(current_buffer, nix::CanonPath::fromCwd(), m_staticEnv);
                        flush_buffer();
                    }
                    catch (const nix::ParseError& e)
                    {
                        std::string error_msg = e.what();
                        if (error_msg.find("unexpected end of file") == std::string::npos
                            && error_msg.find("expression ended unexpectedly") == std::string::npos)
                        {
                            flush_buffer();
                        }
                    }
                    catch (const nix::UndefinedVarError&)
                    {
                        // ignore undefined variable errors as variable might be defined in a preceding chunk of the cell
                    }
                }
            }
            flush_buffer();

            for (size_t i = 0; i < chunks.size(); ++i)
            {
                const auto& chunk = chunks[i];
                if (nix::trim(chunk).empty())
                {
                    continue;
                }

                bool is_last_expression = true;
                if (i < chunks.size() - 1)
                {
                    is_last_expression = false;
                }
                else
                {
                    // an assignment cannot be the final result of a cell
                    try
                    {
                        auto result_variant = m_evaluator->parseReplInput(chunk, nix::CanonPath::fromCwd(), m_staticEnv);
                        if (std::holds_alternative<nix::ExprReplBindings>(result_variant))
                        {
                            is_last_expression = false;
                        }
                    }
                    catch (...)
                    {
                    }
                }
                execute_chunk(chunk, is_last_expression, execution_counter);
            }

            cb(xeus::create_successful_reply());
        }
        // catch and report various types exceptions
        catch (const nix::Interrupted& e)
        {
            send_error("Interrupted", e.what());
        }
        catch (const nix::UndefinedVarError& e)
        {
            send_error("UndefinedVarError", e.what());
        }
        catch (const nix::TypeError& e)
        {
            send_error("TypeError", e.what());
        }
        catch (const nix::ParseError& e)
        {
            send_error("ParseError", e.what());
        }
        catch (const nix::EvalError& e)
        {
            send_error("EvalError", e.what());
        }
        catch (const nix::Error& e)
        {
            send_error("LixError", e.what());
        }
        catch (const std::exception& e)
        {
            send_error("StdException", e.what());
        }
        catch (...)
        {
            send_error("UnknownError", "An unknown error occurred in the kernel.");
        }
    }

    json interpreter::complete_nix_expression(const std::string_view code, int cursor_pos)
    {
        std::vector<std::string> matches;

        auto is_ident_char = [](char c) {
            return isalnum(c) || c == '_' || c == '-' || c == '.' || c == '\'';
        };

        int start = cursor_pos;
        while (start > 0 && is_ident_char(code[start - 1]))
        {
            start--;
        }

        int end = cursor_pos;
        while (static_cast<size_t>(end) < code.length() && is_ident_char(code[end]))
        {
            end++;
        }

        std::string prefix(code.substr(start, cursor_pos - start));

        try
        {
            // handle attribute path completion (e.g. `pkgs.lib.str`)
            size_t last_dot_pos = prefix.rfind('.');
            if (last_dot_pos != std::string::npos)
            {
                std::string base_expr_str = prefix.substr(0, last_dot_pos);
                std::string attr_prefix = prefix.substr(last_dot_pos + 1);

                nix::Value base_val(nix::Value::null_t{});
                eval_pure_expression(base_expr_str, base_val);

                if (base_val.type() == nix::nAttrs)
                {
                    for (const auto& attr : *base_val.attrs)
                    {
                        std::string_view attr_name_sv = m_evaluator->symbols[attr.name];
                        std::string attr_name(attr_name_sv);
                        if (attr_name.rfind(attr_prefix, 0) == 0)
                        {
                            matches.push_back(base_expr_str + "." + attr_name);
                        }
                    }
                }
            }
            else // handle top-level variable completion
            {
                std::set<std::string> candidates;
                // collect variables from the current static environment chain
                std::function<void(const nix::StaticEnv*)> collect_vars =
                    [&](const nix::StaticEnv* senv) {
                    if (!senv) return;
                    for (const auto& [symbol, displ] : senv->vars)
                    {
                        std::string_view var_name_sv = m_evaluator->symbols[symbol];
                        std::string var_name(var_name_sv);
                        if (var_name.rfind("__", 0) != 0) // exclude internal builtins
                        {
                            candidates.insert(var_name);
                        }
                    }
                    if (senv->up) collect_vars(senv->up);
                };
                collect_vars(m_staticEnv.get());

                // find variable names in `let` bindings within the prefix
                size_t let_pos = prefix.rfind("let ");
                size_t in_pos = prefix.rfind(" in ");
                if (let_pos != std::string::npos && (in_pos == std::string::npos || let_pos > in_pos))
                {
                    std::string bindings_str = prefix.substr(let_pos + 4);
                    std::regex var_regex(R"(([a-zA-Z_][a-zA-Z0-9_'-]*)\s*=)");
                    auto vars_begin = std::sregex_iterator(bindings_str.begin(), bindings_str.end(), var_regex);
                    auto vars_end = std::sregex_iterator();
                    for (auto i = vars_begin; i != vars_end; ++i)
                    {
                        candidates.insert((*i)[1].str());
                    }
                }

                for (const auto& var_name : candidates)
                {
                    if (var_name.rfind(prefix, 0) == 0)
                    {
                        matches.push_back(var_name);
                    }
                }
            }
        }
        catch (...)
        {
            // completion can fail, which is fine
        }
        return xeus::create_complete_reply(matches, start, end);
    }

    // main completion request handler  dispatches to REPL or expression completion
    json interpreter::complete_request_impl(const std::string& code, int cursor_pos)
    {
        std::string prefix = code.substr(0, cursor_pos);
        std::string trimmed_prefix = nix::trim(prefix);

        // check for REPL command completion
        if (!trimmed_prefix.empty() && trimmed_prefix[0] == ':')
        {
            auto parts = nix::tokenizeString<std::vector<std::string>>(trimmed_prefix, " \t\n\r");
            std::string cmd = parts.empty() ? "" : parts[0];

            if (parts.size() <= 1 && (prefix.empty() || !std::isspace(prefix.back())))
            {
                std::vector<std::string> matches;
                for (const auto& pair : s_repl_commands)
                {
                    if (pair.first.rfind(cmd, 0) == 0)
                    {
                        matches.push_back(pair.first);
                    }
                }
                size_t start_pos = prefix.find_first_not_of(" \t");
                return xeus::create_complete_reply(matches, start_pos, cursor_pos);
            }

            // for commands that take an expression as an argument, delegate to nix expression completion
            auto it = s_repl_commands.find(cmd);
            if (it != s_repl_commands.end())
            {
                auto handler = it->second;
                if (handler == &interpreter::repl_doc || handler == &interpreter::repl_type
                    || handler == &interpreter::repl_build || handler == &interpreter::repl_add
                    || handler == &interpreter::repl_build_local || handler == &interpreter::repl_load_flake
                    || handler == &interpreter::repl_log || handler == &interpreter::repl_print)
                {
                    size_t arg_start_pos = prefix.find(cmd) + cmd.length();
                    while (arg_start_pos < prefix.length() && std::isspace(prefix[arg_start_pos]))
                    {
                        arg_start_pos++;
                    }

                    std::string arg_code = prefix.substr(arg_start_pos);
                    int arg_cursor_pos = cursor_pos - arg_start_pos;

                    nl::json res = complete_nix_expression(arg_code, arg_cursor_pos);
                    res["cursor_start"] = res["cursor_start"].get<int>() + arg_start_pos;
                    res["cursor_end"] = cursor_pos;
                    return res;
                }
            }
        }

        return complete_nix_expression(code, cursor_pos);
    }

    json interpreter::inspect_request_impl(const std::string& code, int cursor_pos, int)
    {
        try
        {
            auto is_ident_char = [](char c) {
                return isalnum(c) || c == '_' || c == '-' || c == '.' || c == '\'';
            };

            size_t end = cursor_pos;
            while (end < code.length() && is_ident_char(code[end]))
            {
                end++;
            }
            size_t start = cursor_pos;
            while (start > 0 && is_ident_char(code[start - 1]))
            {
                start--;
            }

            std::string to_inspect = code.substr(start, end - start);
            if (to_inspect.empty() || nix::trim(to_inspect).empty())
            {
                return xeus::create_inspect_reply(false);
            }

            nix::Value v(nix::Value::null_t{});
            eval_pure_expression(to_inspect, v);

            std::string doc_str = get_doc_string(v);

            if (!doc_str.empty())
            {
                nl::json data;
                data["text/markdown"] = std::move(doc_str);
                return xeus::create_inspect_reply(true, data);
            }
        }
        catch (...)
        {
            // inspection can fail, which is fine
        }
        return xeus::create_inspect_reply(false);
    }

    json interpreter::is_complete_request_impl(const std::string& code)
    {
        if (nix::trim(code).empty())
        {
            return xeus::create_is_complete_reply("complete");
        }
        try
        {
            (void)m_evaluator->parseReplInput(code, nix::CanonPath::fromCwd(), m_staticEnv);
            return xeus::create_is_complete_reply("complete");
        }
        catch (const nix::ParseError& e)
        {
            std::string error_msg = e.what();
            if (error_msg.find("unexpected end of file") != std::string::npos
                || error_msg.find("expression ended unexpectedly") != std::string::npos)
            {
                return xeus::create_is_complete_reply("incomplete");
            }
            return xeus::create_is_complete_reply("invalid");
        }
        catch (...)
        {
            return xeus::create_is_complete_reply("unknown");
        }
    }

    json interpreter::kernel_info_request_impl()
    {
        return xeus::create_info_reply(
            "5.3",            // protocol_version
            "xeus-lix",       // implementation
            "0.1.0",          // implementation_version
            "nix",            // language_name
            nix::nixVersion,  // language_version
            "text/x-nix",     // language_mimetype
            ".nix"            // language_file_extension
        );
    }
}
