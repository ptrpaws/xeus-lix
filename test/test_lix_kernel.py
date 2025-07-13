import unittest
import jupyter_kernel_test
from jupyter_kernel_test import TIMEOUT, validate_message
import re
import os
import shutil

class LixKernelTests(jupyter_kernel_test.KernelTests):
    kernel_name = "lix"
    language_name = "nix"

    @classmethod
    def setUpClass(cls):
        # create test.nix for :load tests
        os.makedirs("test", exist_ok=True)
        with open("test/test.nix", "w") as f:
            f.write('{ message = "hello from file"; value = 42; }')

        # create a test flake for :lf tests inside the test directory
        os.makedirs("test/test_flake", exist_ok=True)
        with open("test/test_flake/flake.nix", "w") as f:
            f.write('''
            {
              description = "A test flake for xeus-lix";
              outputs = { self }: {
                some_value = 12345;
              };
            }
            ''')
        super().setUpClass()
        # pre-load nixpkgs for build tests
        cls.kc.execute(':l <nixpkgs>')
        cls.flush_channels(cls)


    @classmethod
    def tearDownClass(cls):
        if os.path.exists("test/test.nix"):
            os.remove("test/test.nix")
        if os.path.exists("test/test_flake"):
            shutil.rmtree("test/test_flake")
        if os.path.lexists("result-out"):
            os.remove("result-out")
        super().tearDownClass()

    # helper function to reliably get the completion reply
    def get_completion_reply(self, msg_id, timeout=TIMEOUT):
        while True:
            msg = self.kc.get_shell_msg(timeout=timeout)
            if msg['parent_header']['msg_id'] == msg_id and msg['header']['msg_type'] == 'complete_reply':
                return msg

    def _strip_ansi(self, text):
        if not text:
            return ""
        ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
        return ansi_escape.sub('', text)

    # overridden test methods

    def test_error(self):
        self.flush_channels()
        reply, output_msgs = self.execute_helper(code=self.code_generate_error)
        self.assertEqual(reply['content']['status'], 'error')
        self.assertEqual(reply['content']['ename'], 'UndefinedVarError')
        self.assertGreaterEqual(len(output_msgs), 1)
        self.assertEqual(output_msgs[0]['header']['msg_type'], 'error')
        self.assertEqual(output_msgs[0]['content']['ename'], 'UndefinedVarError')
        self.assertIn('undefined variable', output_msgs[0]['content']['evalue'])

    def test_execute_result(self):
        if not self.code_execute_result:
            raise unittest.SkipTest("No code_execute_result samples")

        for sample in self.code_execute_result:
            with self.subTest(code=sample["code"]):
                self.flush_channels()
                reply, output_msgs = self.execute_helper(sample["code"])
                self.assertEqual(reply["content"]["status"], "ok")
                self.assertGreaterEqual(len(output_msgs), 1, "No output messages received")

                found = False
                for msg in output_msgs:
                    if msg["msg_type"] == "execute_result":
                        found = True
                        mime = sample.get("mime", "text/plain")
                        self.assertIn(mime, msg["content"]["data"])
                        if "result" in sample:
                            cleaned_result = self._strip_ansi(msg["content"]["data"][mime]).strip()
                            self.assertEqual(cleaned_result, sample["result"])
                if not found:
                    self.fail("execute_result message not found")

    def test_execute_stderr(self):
        if not self.code_execute_stderr:
            raise unittest.SkipTest('No code stderr')

        self.flush_channels()
        reply, output_msgs = self.execute_helper(code=self.code_execute_stderr)

        self.assertEqual(reply['content']['status'], 'ok')
        self.assertGreaterEqual(len(output_msgs), 1)

        found_stderr = False
        for msg in output_msgs:
            if msg['header']['msg_type'] == 'stream' and msg['content']['name'] == 'stderr':
                found_stderr = True
                self.assertIn('this is a trace', msg['content']['text'])
        self.assertTrue(found_stderr, 'no stderr stream found')

    def test_completion(self):
        if not self.code_completion_samples:
            raise unittest.SkipTest('No completion samples')

        for sample in self.code_completion_samples:
            with self.subTest(code=sample["code"]):
                self.flush_channels()
                msg_id = self.kc.complete(sample["code"])
                reply = self.get_completion_reply(msg_id)
                validate_message(reply, "complete_reply", msg_id)
                self.assertEqual(reply["content"]["status"], "ok")
                self.assertTrue(
                    any(sample["matches"] in s for s in reply["content"]["matches"]),
                    f"'{sample['matches']}' not found in {reply['content']['matches']}"
                )

    def test_inspect(self):
        if not self.code_inspect_sample:
            raise unittest.SkipTest("No code_inspect_sample")

        self.flush_channels()
        msg_id = self.kc.inspect(self.code_inspect_sample['code'])
        reply = self.get_non_kernel_info_reply(timeout=TIMEOUT)
        validate_message(reply, "inspect_reply", msg_id)
        self.assertEqual(reply["content"]["status"], "ok")
        self.assertTrue(reply["content"]["found"])
        self.assertIn("text/markdown", reply["content"]["data"])
        self.assertIn(self.code_inspect_sample['result'], reply["content"]["data"]["text/markdown"])

    # test case data

    code_hello_world = '!echo "hello, world"'
    code_execute_result = [
        {'code': '100 * 5', 'result': '500'},
        {'code': '"foo" + "bar"', 'result': '"foobar"'}
    ]
    code_execute_stderr = 'builtins.trace "this is a trace" 1'

    code_completion_samples = [
        {'code': 'builtins.toJ', 'matches': 'toJSON'},
        {'code': ':l', 'matches': ':load'},
        {'code': ':b', 'matches': ':b'},
        {'code': ':p', 'matches': ':print'},
    ]

    code_generate_error = "an_undefined_variable"
    code_inspect_sample = {
        'code': 'builtins.map',
        'result': '**Synopsis:** `builtins.map`'
    }

    # custom tests

    def test_lix_load_command(self):
        self.flush_channels()
        reply, output_msgs = self.execute_helper(code=':load ./test/test.nix')
        self.assertEqual(reply['content']['status'], 'ok')
        self.assertGreaterEqual(len(output_msgs), 1)
        self.assertIn("Added", output_msgs[0]['content']['text'])

        self.flush_channels()
        reply, output_msgs = self.execute_helper(code='message')
        self.assertIn("hello from file", self._strip_ansi(output_msgs[0]['content']['data']['text/plain']))

    def test_lix_add_command(self):
        self.flush_channels()
        reply, output_msgs = self.execute_helper(code=':add { new_var = 123; }')
        self.assertEqual(reply['content']['status'], 'ok')
        self.assertGreaterEqual(len(output_msgs), 1)
        self.assertIn("Added 1 variables", output_msgs[0]['content']['text'])

        self.flush_channels()
        reply, output_msgs = self.execute_helper(code='new_var')
        self.assertIn("123", self._strip_ansi(output_msgs[0]['content']['data']['text/plain']))

    def test_lix_build_command(self):
        self.flush_channels()
        code = ':b pkgs.runCommand "test-drv" {} "echo hello-build > $out"'
        reply, output_msgs = self.execute_helper(code=code)

        self.assertEqual(reply['content']['status'], 'ok')
        stdout = "".join([msg['content']['text'] for msg in output_msgs if msg['header']['msg_type'] == 'stream' and msg['content']['name'] == 'stdout'])
        self.assertIn("Building", stdout)
        self.assertIn("This derivation produced the following outputs:", stdout)
        self.assertIn("out -> /nix/store/", stdout)

    def test_lix_build_local_command(self):
        self.flush_channels()
        if os.path.lexists("result-out"):
            os.remove("result-out")

        code = ':bl pkgs.runCommand "test-local-drv" {} "echo hello-local > $out"'
        reply, output_msgs = self.execute_helper(code=code)

        self.assertEqual(reply['content']['status'], 'ok')
        stdout = "".join([msg['content']['text'] for msg in output_msgs if msg['header']['msg_type'] == 'stream' and msg['content']['name'] == 'stdout'])
        self.assertIn("Building", stdout)
        self.assertIn("This derivation produced the following outputs:", stdout)
        self.assertIn("./result-out -> /nix/store/", stdout)
        self.assertTrue(os.path.lexists("result-out"))
        os.remove("result-out")

    def test_rich_display(self):
        self.flush_channels()
        code = """
        {
          _toMime = {
            "text/html" = "<h1>Hello HTML</h1>";
            "text/plain" = "Hello Plain";
          };
        }
        """
        reply, output_msgs = self.execute_helper(code=code)

        self.assertEqual(reply['content']['status'], 'ok')
        self.assertGreaterEqual(len(output_msgs), 1, "No output messages received")

        found = False
        for msg in output_msgs:
            if msg["msg_type"] == "execute_result":
                found = True
                self.assertIn("text/html", msg["content"]["data"])
                self.assertEqual(msg["content"]["data"]["text/html"], "<h1>Hello HTML</h1>")
                self.assertIn("text/plain", msg["content"]["data"])
                self.assertEqual(msg["content"]["data"]["text/plain"], "Hello Plain")
        if not found:
            self.fail("execute_result message not found for rich display test")

    def test_lix_load_flake_command(self):
        self.flush_channels()
        reply, output_msgs = self.execute_helper(code=':lf path:./test/test_flake')
        self.assertEqual(reply['content']['status'], 'ok')

        # search for the "Added" message on stdout, ignoring stderr fetching messages
        stdout_msgs = [msg['content']['text'] for msg in output_msgs if msg['header']['msg_type'] == 'stream' and msg['content']['name'] == 'stdout']
        self.assertTrue(any("Added" in s for s in stdout_msgs), "Did not find 'Added' in stdout messages")

        self.flush_channels()
        reply, output_msgs = self.execute_helper(code='some_value')
        self.assertIn("12345", self._strip_ansi(output_msgs[0]['content']['data']['text/plain']))

    def test_shell_command(self):
        self.flush_channels()
        reply, output_msgs = self.execute_helper(code='!echo "hello from shell"')

        self.assertEqual(reply['content']['status'], 'ok')
        self.assertGreaterEqual(len(output_msgs), 1)

        found_stream = False
        for msg in output_msgs:
            if msg['header']['msg_type'] == 'stream' and msg['content']['name'] == 'stdout':
                found_stream = True
                self.assertIn('hello from shell', msg['content']['text'])
        self.assertTrue(found_stream, 'no stdout stream found for shell command')

    def test_is_complete(self):
        def check_is_complete(code, expected_status):
            with self.subTest(code=code, expected_status=expected_status):
                self.flush_channels()
                msg = self.kc.session.msg('is_complete_request', {'code': code})
                msg_id = msg['header']['msg_id']

                self.kc.shell_channel.send(msg)

                reply = self.kc.get_shell_msg(timeout=TIMEOUT)

                validate_message(reply, 'is_complete_reply', msg_id)
                self.assertEqual(reply['content']['status'], expected_status)

        check_is_complete("1 + 1", 'complete')
        check_is_complete("let a =", 'incomplete')
        check_is_complete("a b c d;", 'invalid')

    def test_lix_print_command(self):
        self.flush_channels()
        code = ':p { a = 1; b = "foo"; }'
        reply, output_msgs = self.execute_helper(code=code)

        self.assertEqual(reply['content']['status'], 'ok')
        self.assertGreaterEqual(len(output_msgs), 1)

        stdout = "".join([msg['content']['text'] for msg in output_msgs if msg['header']['msg_type'] == 'stream' and msg['content']['name'] == 'stdout'])
        clean_stdout = self._strip_ansi(stdout).strip()
        expected_output = '{\n  a = 1;\n  b = "foo";\n}'
        self.assertEqual(clean_stdout, expected_output)

    def test_lix_type_command(self):
        self.flush_channels()
        reply, output_msgs = self.execute_helper(code=':t 123')
        self.assertEqual(reply['content']['status'], 'ok')
        self.assertGreaterEqual(len(output_msgs), 1)
        self.assertIn("an integer", output_msgs[0]['content']['text'])

if __name__ == "__main__":
    unittest.main()
