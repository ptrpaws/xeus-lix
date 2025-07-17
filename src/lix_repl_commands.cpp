#include "lix_interpreter.hpp"

#include "lix/config.h"
#include "lix/libcmd/common-eval-args.hh"
#include "lix/libexpr/attr-set.hh"
#include "lix/libexpr/eval-settings.hh"
#include "lix/libexpr/eval.hh"
#include "lix/libexpr/flake/flake.hh"
#include "lix/libexpr/flake/lockfile.hh"
#include "lix/libexpr/get-drvs.hh"
#include "lix/libexpr/print.hh"
#include "lix/libexpr/value.hh"
#include "lix/libstore/globals.hh"
#include "lix/libstore/local-fs-store.hh"
#include "lix/libstore/log-store.hh"
#include "lix/libstore/store-api.hh"
#include "lix/libutil/error.hh"
#include "lix/libutil/strings.hh"

namespace xeus_lix
{
    const std::map<std::string, interpreter::repl_command_handler> interpreter::s_repl_commands = {
        { ":doc", &interpreter::repl_doc },
        { ":t", &interpreter::repl_type },
        { ":l", &interpreter::repl_load },
        { ":load", &interpreter::repl_load },
        { ":b", &interpreter::repl_build },
        { ":a", &interpreter::repl_add },
        { ":add", &interpreter::repl_add },
        { ":?", &interpreter::repl_help },
        { ":help", &interpreter::repl_help },
        { ":r", &interpreter::repl_reload },
        { ":reload", &interpreter::repl_reload },
        { ":bl", &interpreter::repl_build_local },
        { ":env", &interpreter::repl_env },
        { ":lf", &interpreter::repl_load_flake },
        { ":load-flake", &interpreter::repl_load_flake },
        { ":p", &interpreter::repl_print },
        { ":print", &interpreter::repl_print },
        { ":log", &interpreter::repl_log },
        { ":te", &interpreter::repl_trace_enable },
        { ":trace-enable", &interpreter::repl_trace_enable },
    };

    void interpreter::handle_repl_command(const std::string& command_line)
    {
        std::string command;
        std::string arg;
        size_t p = command_line.find_first_of(" \n\r\t");
        if (p != std::string::npos)
        {
            command = command_line.substr(0, p);
            arg = nix::trim(command_line.substr(p));
        }
        else
        {
            command = command_line;
        }

        auto it = s_repl_commands.find(command);
        if (it != s_repl_commands.end())
        {
            (this->*(it->second))(arg);
        }
        else
        {
            publish_stream("stderr", "Unknown REPL command: '" + command + "'. Type :help for a list of commands.\n");
        }
    }

    // :doc <expr> - Show documentation for the provided value
    void interpreter::repl_doc(const std::string& arg)
    {
        nix::Value v(nix::Value::null_t{});
        eval_pure_expression(arg, v);
        std::string doc_str = get_doc_string(v);
        if (!doc_str.empty())
        {
            nl::json bundle;
            bundle["text/markdown"] = std::move(doc_str);
            display_data(std::move(bundle), nl::json::object(), nl::json::object());
        }
        else
        {
            publish_stream("stderr", "Value does not have documentation.\n");
        }
    }

    // :t <expr> - Describe result of evaluation
    void interpreter::repl_type(const std::string& arg)
    {
        nix::Value v(nix::Value::null_t{});
        eval_pure_expression(arg, v);
        publish_stream("stdout", nix::showType(v) + "\n");
    }

    // :load <path> - Load Nix expression and add it to scope
    void interpreter::repl_load(const std::string& arg)
    {
        auto path = m_aio->blockOn(nix::lookupFileArg(*m_evaluator, arg)).unwrap();
        nix::Value v(nix::Value::null_t{}), v_autocalled(nix::Value::null_t{});
        m_evalState->evalFile(path, v);
        nix::Bindings* auto_args = m_evaluator->buildBindings(0).finish();
        m_evalState->autoCallFunction(*auto_args, v, v_autocalled, nix::noPos);
        m_evalState->forceAttrs(v_autocalled, nix::noPos, "while loading file attributes");
        add_to_scope(*v_autocalled.attrs);
        m_loaded_files.push_back(path.to_string());
    }

    // :reload - Reload all files
    void interpreter::repl_reload(const std::string& arg)
    {
        if (!arg.empty())
        {
            publish_stream("stderr", ":reload does not take any arguments.\n");
            return;
        }
        publish_stream("stdout", "Reloading environment...\n");
        m_evalState->resetFileCache();

        initialize_scope();

        auto files_to_reload = m_loaded_files;
        m_loaded_files.clear();

        if (files_to_reload.empty())
        {
            publish_stream("stdout", "No files to reload.\n");
            return;
        }

        for (const auto& path : files_to_reload)
        {
            try
            {
                publish_stream("stdout", "Reloading " + path + "\n");
                repl_load(path);
            }
            catch (const std::exception& e)
            {
                publish_stream("stderr", "Failed to reload " + path + ": " + e.what() + "\n");
            }
        }
    }

    // :add <expr> - Add attributes from resulting set to scope
    void interpreter::repl_add(const std::string& arg)
    {
        nix::Value v(nix::Value::null_t{});
        eval_pure_expression(arg, v);
        m_evalState->forceAttrs(v, nix::noPos, "while evaluating attribute set for :add");
        add_to_scope(*v.attrs);
    }

    // :b <expr> - Build a derivation
    void interpreter::repl_build(const std::string& arg)
    {
        nix::Value v(nix::Value::null_t{});
        eval_pure_expression(arg, v);
        auto drvInfo = nix::getDerivation(*m_evalState, v, false);
        if (!drvInfo)
        {
            throw nix::Error("expression does not evaluate to a derivation.");
        }
        auto drvPath = drvInfo->queryDrvPath(*m_evalState);
        if (!drvPath)
        {
            throw nix::Error("derivation is missing 'drvPath' attribute.");
        }
        publish_stream("stdout", "Building " + m_store->printStorePath(*drvPath) + "\n");
        m_aio->blockOn(m_store->buildPaths(
            { nix::DerivedPath::Built{ .drvPath = nix::makeConstantStorePath(*drvPath), .outputs = nix::OutputsSpec::All{} } }
        ));
        publish_stream("stdout", "\nThis derivation produced the following outputs:");
        for (auto& [outputName, outputPath] : m_aio->blockOn(m_store->queryDerivationOutputMap(*drvPath)))
        {
            publish_stream("stdout", "\n  " + outputName + " -> " + m_store->printStorePath(outputPath));
        }
        publish_stream("stdout", "\n");
    }

    // :bl <expr> - Build a derivation, creating GC roots in the working directory
    void interpreter::repl_build_local(const std::string& arg)
    {
        nix::Value v(nix::Value::null_t{});
        eval_pure_expression(arg, v);
        auto drvInfo = nix::getDerivation(*m_evalState, v, false);
        if (!drvInfo)
        {
            throw nix::Error("expression does not evaluate to a derivation.");
        }
        auto drvPath = drvInfo->queryDrvPath(*m_evalState);
        if (!drvPath)
        {
            throw nix::Error("derivation is missing 'drvPath' attribute.");
        }

        publish_stream("stdout", "Building " + m_store->printStorePath(*drvPath) + "\n");
        m_aio->blockOn(m_store->buildPaths(
            { nix::DerivedPath::Built{ .drvPath = nix::makeConstantStorePath(*drvPath), .outputs = nix::OutputsSpec::All{} } }
        ));

        auto localStore = m_store.try_cast_shared<nix::LocalFSStore>();
        if (!localStore)
        {
            publish_stream(
                "stderr",
                "Warning: store is not a local filesystem store, cannot create GC roots in the working directory.\n"
            );
        }

        publish_stream("stdout", "\nThis derivation produced the following outputs:\n");
        for (auto& [outputName, outputPath] : m_aio->blockOn(m_store->queryDerivationOutputMap(*drvPath)))
        {
            if (localStore)
            {
                std::string symlink = "result-" + outputName;
                try
                {
                    m_aio->blockOn(localStore->addPermRoot(outputPath, nix::absPath(symlink)));
                    publish_stream("stdout", "  ./" + symlink + " -> " + m_store->printStorePath(outputPath) + "\n");
                }
                catch (const std::exception& e)
                {
                    publish_stream("stderr", "Could not create symlink '" + symlink + "': " + e.what() + "\n");
                }
            }
            else
            {
                publish_stream("stdout", "  " + outputName + " -> " + m_store->printStorePath(outputPath) + "\n");
            }
        }
    }

    // :env - Show variables in the current scope
    void interpreter::repl_env(const std::string& /* arg */)
    {
        std::stringstream ss;

        std::function<void(const nix::StaticEnv*, int)> print_level =
            [&](const nix::StaticEnv* senv, int level) {

            if (!senv) {
                return;
            }

            ss << "Env level " << level << "\n";

            if (senv->up) {
                std::vector<std::string_view> var_names;
                for (const auto& [symbol, displ] : senv->vars) {
                    var_names.push_back(m_evaluator->symbols[symbol]);
                }
                std::sort(var_names.begin(), var_names.end());

                ss << "static: " << ANSI_MAGENTA;
                for (const auto& name : var_names) {
                    ss << name << " ";
                }
                ss << ANSI_NORMAL << "\n\n";

                // recurse to the next level
                print_level(senv->up, level + 1);
            } else {
                std::vector<std::string_view> var_names;
                for (const auto& [symbol, displ] : senv->vars) {
                     std::string_view name = m_evaluator->symbols[symbol];
                     if (!name.starts_with("__")) { // exclude internal builtins
                        var_names.push_back(name);
                     }
                }
                std::sort(var_names.begin(), var_names.end());

                ss << ANSI_MAGENTA;
                for (const auto& name : var_names) {
                    ss << name << " ";
                }
                ss << ANSI_NORMAL << "\n";
            }
        };

        print_level(m_staticEnv.get(), 0);

        publish_stream("stdout", ss.str());
    }

    // :load-flake <ref> - Load Nix flake and add it to scope
    void interpreter::repl_load_flake(const std::string& arg)
    {
        if (arg.empty())
        {
            publish_stream("stderr", ":lf requires a flake reference.\n");
            return;
        }
        auto flakeRef = nix::parseFlakeRef(std::string(arg), nix::absPath("."), true);
        nix::Value v(nix::Value::null_t{});
        nix::flake::callFlake(
            *m_evalState,
            nix::flake::lockFlake(
                *m_evalState,
                flakeRef,
                nix::flake::LockFlags{ .updateLockFile = false,
                                       .useRegistries = !nix::evalSettings.pureEval.get(),
                                       .allowUnlocked = !nix::evalSettings.pureEval.get() }
            ),
            v
        );
        m_evalState->forceAttrs(v, nix::noPos, "while loading flake outputs");
        add_to_scope(*v.attrs);
    }

    // :p <expr> - Evaluate and print expression recursively Strings are printed directly, without escaping.
    void interpreter::repl_print(const std::string& arg)
    {
        nix::Value v(nix::Value::null_t{});
        eval_pure_expression(arg, v);

        if (v.type() == nix::nString)
        {
            publish_stream("stdout", v.string.s);
        }
        else
        {
            std::stringstream ss;
            nix::printValue(
                *m_evalState,
                ss,
                v,
                nix::PrintOptions{ .ansiColors = true,
                                   .force = true,
                                   .derivationPaths = true,
                                   .maxDepth = std::numeric_limits<unsigned int>::max(),
                                   .prettyIndent = 2 }
            );
            publish_stream("stdout", ss.str() + "\n");
        }
    }

    // :log <expr | .drv path> - Show logs for a derivation
    void interpreter::repl_log(const std::string& arg)
    {
        if (arg.empty())
        {
            throw nix::Error(":log requires a derivation path or an expression");
        }

        nix::StorePath drvPath = ([&] {
            if (auto maybeDrvPath = m_store->maybeParseStorePath(std::string(arg));
                maybeDrvPath && maybeDrvPath->isDerivation())
            {
                return *maybeDrvPath;
            }
            else
            {
                nix::Value v(nix::Value::null_t{});
                eval_pure_expression(arg, v);
                auto drvInfo = nix::getDerivation(*m_evalState, v, false);
                if (!drvInfo)
                {
                    throw nix::Error("expression does not evaluate to a derivation.");
                }
                auto drvPath = drvInfo->queryDrvPath(*m_evalState);
                if (!drvPath)
                {
                    throw nix::Error("derivation is missing 'drvPath' attribute.");
                }
                return *drvPath;
            }
        })();

        auto drvPathRaw = m_store->printStorePath(drvPath);

        auto subs = m_aio->blockOn(nix::getDefaultSubstituters());
        subs.push_front(m_store);

        bool foundLog = false;
        for (auto& sub : subs)
        {
            auto logSub = dynamic_cast<nix::LogStore*>(&*sub);
            if (!logSub)
            {
                continue;
            }

            auto log = m_aio->blockOn(logSub->getBuildLog(drvPath));
            if (log)
            {
                publish_stream("stdout", "Log for " + drvPathRaw + " from " + sub->getUri() + ":\n" + *log);
                foundLog = true;
                break;
            }
        }

        if (!foundLog)
        {
            publish_stream("stderr", "No build log found for " + drvPathRaw + "\n");
        }
    }

    // :te [bool] - Enable, disable or toggle showing traces for errors
    void interpreter::repl_trace_enable(const std::string& arg)
    {
        bool current = nix::loggerSettings.showTrace.get();
        bool next;
        if (arg == "true")
        {
            next = true;
        }
        else if (arg == "false")
        {
            next = false;
        }
        else if (arg.empty())
        {
            next = !current;
        }
        else
        {
            publish_stream("stderr", "Invalid argument to :te. Expected 'true', 'false', or nothing.\n");
            return;
        }
        nix::loggerSettings.showTrace.override(next);
        publish_stream("stdout", std::string("Error traces are now ") + (next ? "enabled.\n" : "disabled.\n"));
    }

    // :help - Brings up this help menu
    void interpreter::repl_help(const std::string& /* arg */)
    {
        const std::string help_markdown = R"md(The following commands are available:

```
  <expr>                       Evaluate and print expression
  <x> = <expr>                 Bind expression to variable
  :a, :add <expr>              Add attributes from resulting set to scope
  :b <expr>                    Build a derivation
  :bl <expr>                   Build a derivation, creating GC roots
                               in the working directory
  :env                         Show variables in the current scope
  :doc <expr>                  Show documentation for the provided value
  :l, :load <path>             Load Nix expression and add it to scope
  :lf, :load-flake <ref>       Load Nix flake and add it to scope
  :p, :print <expr>            Evaluate and print expression recursively
                               Strings are printed directly, without escaping.
  :r, :reload                  Reload all files
  :t <expr>                    Describe result of evaluation
  :log <expr | .drv path>      Show logs for a derivation
  :te, :trace-enable [bool]    Enable, disable or toggle showing traces for
                               errors
  :?, :help                    Brings up this help menu
```
)md";

        nl::json bundle;
        bundle["text/markdown"] = help_markdown;
        display_data(std::move(bundle), nl::json::object(), nl::json::object());
    }
}
