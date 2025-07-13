#include "lix_interpreter.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <string_view>

#include "lix/libcmd/markdown.hh"
#include "lix/libexpr/eval.hh"
#include "lix/libutil/ansicolor.hh"
#include "lix/libutil/canon-path.hh"
#include "lix/libutil/error.hh"
#include "lix/libutil/strings.hh"

namespace xeus_lix
{
    void interpreter::execute_shell_command(const std::string_view command_block)
    {
        std::stringstream script_stream;
        std::istringstream block_stream{ std::string(command_block) };
        std::string line;

        // extract the command from each line & strip leading '!'
        while (std::getline(block_stream, line))
        {
            std::string_view sv = line;
            size_t first_char_pos = sv.find_first_not_of(" \t");
            if (first_char_pos != std::string_view::npos && sv[first_char_pos] == '!')
            {
                sv.remove_prefix(first_char_pos + 1);
                // strip leading space after the '!'
                if (!sv.empty() && sv.front() == ' ')
                {
                    sv.remove_prefix(1);
                }
            }
            script_stream << sv << '\n';
        }

        std::string cmd_to_run = script_stream.str();
        if (nix::trim(cmd_to_run).empty())
        {
            return;
        }

        // redirect stderr to stdout to capture all output in the cell
        cmd_to_run += " 2>&1";

        std::array<char, 128> buffer;
        std::string result;

        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd_to_run.c_str(), "r"), pclose);
        if (!pipe)
        {
            publish_stream("stderr", "popen() failed!\n");
            return;
        }
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        {
            result += buffer.data();
        }

        if (!result.empty())
        {
            publish_stream("stdout", result);
        }
    }

    void interpreter::add_to_scope(nix::Bindings& bindings)
    {
        if (bindings.empty())
        {
            return;
        }
        if (m_displacement + bindings.size() > NIX_ENV_SIZE)
        {
            throw std::runtime_error("Nix environment scope overflow. Cannot add more variables.");
        }
        // add each attribute to the static environment (for name lookup) and place its value in the local environment array
        for (auto& attr : bindings)
        {
            m_staticEnv->vars.insert_or_assign(attr.name, m_displacement);
            m_localEnv->values[m_displacement++] = attr.value;
        }
        std::stringstream ss;
        ss << "Added " << bindings.size() << " variables.\n";
        publish_stream("stdout", ss.str());
    }

    void interpreter::eval_pure_expression(const std::string_view expr_str, nix::Value& result)
    {
        if (nix::trim(expr_str).empty())
        {
            result.mkNull();
            return;
        }
        // parse and evaluate the expression within the current environment
        auto& expr = m_evaluator->parseExprFromString(std::string(expr_str), nix::CanonPath::fromCwd(), m_staticEnv);
        expr.eval(*m_evalState, *m_localEnv, result);
        // force the result to a weak head normal form
        m_evalState->forceValue(result, nix::noPos);
    }

    std::string interpreter::get_doc_string(const nix::Value& v) const
    {
        if (auto doc = m_evaluator->builtins.getDoc(const_cast<nix::Value&>(v)))
        {
            std::stringstream markdown_stream;

            if (doc->name)
            {
                markdown_stream << "**Synopsis:** `builtins." << *doc->name << "`";
                for (const auto& arg_name : doc->args)
                {
                    markdown_stream << " *" << arg_name << "*";
                }
                markdown_stream << "\n\n";
            }

            markdown_stream << nix::stripIndentation(doc->doc);
            return markdown_stream.str();
        }
        return "";
    }
}
