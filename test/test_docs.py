#!/usr/bin/env python3
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
"""Checks that the documentation matches the source code: METRICS.md describes exactly the metrics, which are declared
by the server, and the documentation mentions only existing command line options.

Usage: python3 test/test_docs.py
"""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = ['README.md', 'RESTARTS.md', 'METRICS.md', 'VERIFICATION.md', os.path.join('td-patches', 'README.md')]


def read(path):
    with open(os.path.join(ROOT, path)) as f:
        return f.read()


class DocsTest(unittest.TestCase):
    def test_metrics_are_documented(self):
        declared = set()
        for source in ['BotMetrics.cpp', 'ClientManagerMetrics.cpp']:
            declared |= set(re.findall(r'declare\("(tgbotapi_\w+)"', read(os.path.join('telegram-bot-api', source))))
        self.assertGreater(len(declared), 50)
        # histogram samples are documented by their family names
        documented = {re.sub(r'_(bucket|sum|count)$', '', name) if name not in declared else name
                      for name in re.findall(r'\b(tgbotapi_\w+)', read('METRICS.md'))}
        self.assertEqual(declared - documented, set(), 'undocumented metrics')
        self.assertEqual(documented - declared, set(), 'documented metrics, which do not exist')

    def test_documented_options_exist(self):
        # options of the server and of the verification tools, and words used in instructions for git and cmake
        options = set(re.findall(r'"([a-z][a-z0-9-]+)"', read(os.path.join('telegram-bot-api', 'telegram-bot-api.cpp'))))
        for tool in ['latency_ab.py', 'verify_memory_accuracy.py']:
            options |= set(re.findall(r"'--([a-z][a-z0-9-]+)'", read(os.path.join('test', 'verify', tool))))
        options |= {'recursive', 'build', 'target', 'git'}  # 'diff --git' is a header of patches
        for doc in DOCS:
            for option in re.findall(r'(?<![\w-])--([a-z][a-z0-9-]+)', read(doc)):
                self.assertIn(option, options, '%s mentions unknown option --%s' % (doc, option))


if __name__ == '__main__':
    unittest.main()
