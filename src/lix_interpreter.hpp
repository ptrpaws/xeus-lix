#ifndef XEUS_LIX_INTERPRETER_HPP
#define XEUS_LIX_INTERPRETER_HPP

#include "nlohmann/json.hpp"
#include "xeus/xinterpreter.hpp"

#include <lix/libutil/box_ptr.hh>
#include <lix/libutil/ref.hh>

#include <string_view>

// forward declarations for Lix types to reduce header dependencies.
namespace nix
{
class AsyncIoRoot;
struct Bindings;
struct Doc;
struct Env;
struct EvalState;
class Evaluator;
class Logger;
struct StaticEnv;
class Store;
struct Value;
}

namespace xeus_lix
{
    using json = nlohmann::json;

    class interpreter : public xeus::xinterpreter
    {
    public:
        interpreter();
        virtual ~interpreter();

        // the logger needs to access the interpreter's publishing methods
        friend class JupyterLogger;

    private:
        // xeus::xinterpreter implementation
        void configure_impl() override;

        void execute_request_impl(
            send_reply_callback cb,
            int execution_counter,
            const std::string& code,
            xeus::execute_request_config config,
            json user_expressions
        ) override;

        json complete_request_impl(const std::string& code, int cursor_pos) override;
        json inspect_request_impl(const std::string& code, int cursor_pos, int detail_level) override;
        json is_complete_request_impl(const std::string& code) override;
        json kernel_info_request_impl() override;
        void shutdown_request_impl() override;

        // helper Functions
        void execute_chunk(const std::string& chunk, bool is_last_chunk, int execution_counter);
        void execute_shell_command(std::string_view command_block);
        void handle_repl_command(const std::string& command_line);
        void add_to_scope(nix::Bindings& bindings);
        void eval_pure_expression(std::string_view expr_str, nix::Value& result);
        std::string get_doc_string(const nix::Value& v) const;
        json complete_nix_expression(std::string_view code, int cursor_pos);
        void initialize_scope();

        // REPL command handlers
        using repl_command_handler = void (interpreter::*)(const std::string&);
        static const std::map<std::string, repl_command_handler> s_repl_commands;

        void repl_doc(const std::string& arg);
        void repl_type(const std::string& arg);
        void repl_load(const std::string& arg);
        void repl_build(const std::string& arg);
        void repl_add(const std::string& arg);
        void repl_help(const std::string& arg);
        void repl_reload(const std::string& arg);
        void repl_build_local(const std::string& arg);
        void repl_env(const std::string& arg);
        void repl_load_flake(const std::string& arg);
        void repl_print(const std::string& arg);
        void repl_log(const std::string& arg);
        void repl_trace_enable(const std::string& arg);

        // lix evaluation state
        std::unique_ptr<nix::AsyncIoRoot> m_aio;
        nix::ref<nix::Store> m_store;
        std::unique_ptr<nix::Evaluator> m_evaluator;
        nix::box_ptr<nix::EvalState> m_evalState;
        nix::Env* m_localEnv;
        std::shared_ptr<nix::StaticEnv> m_staticEnv;
        int m_displacement;
        std::unique_ptr<nix::Logger> m_logger;
        std::vector<std::string> m_loaded_files;

        // the maximum number of variables (8MiB) that can be stored in the REPL environment
        // https://git.lix.systems/lix-project/lix/src/commit/ae00b1298353a43a10bbecea8220471731db10ec/lix/libcmd/repl.cc#L127
        static const size_t NIX_ENV_SIZE = 1 << 20;
    };
}

#endif
