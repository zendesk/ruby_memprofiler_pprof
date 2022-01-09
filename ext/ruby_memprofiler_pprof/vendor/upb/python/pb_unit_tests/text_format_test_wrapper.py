# Copyright (c) 2009-2021, Google LLC
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of Google LLC nor the
#       names of its contributors may be used to endorse or promote products
#       derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from google.protobuf.internal import text_format_test
import unittest
from google.protobuf.internal import _parameterized

sep = _parameterized._SEPARATOR

text_format_test.OnlyWorksWithProto2RightNowTests.testMergeLinesGolden.__unittest_expecting_failure__ = True
text_format_test.OnlyWorksWithProto2RightNowTests.testParseGolden.__unittest_expecting_failure__ = True
text_format_test.OnlyWorksWithProto2RightNowTests.testParseLinesGolden.__unittest_expecting_failure__ = True
text_format_test.OnlyWorksWithProto2RightNowTests.testPrintAllFields.__unittest_expecting_failure__ = True
text_format_test.OnlyWorksWithProto2RightNowTests.testPrintAllFieldsPointy.__unittest_expecting_failure__ = True
text_format_test.OnlyWorksWithProto2RightNowTests.testPrintInIndexOrder.__unittest_expecting_failure__ = True
text_format_test.OnlyWorksWithProto2RightNowTests.testPrintMapUsingCppImplementation.__unittest_expecting_failure__ = True
text_format_test.OnlyWorksWithProto2RightNowTests.testPrintUnknownFields.__unittest_expecting_failure__ = True
text_format_test.Proto2Tests.testParseGoldenExtensions.__unittest_expecting_failure__ = True
text_format_test.Proto2Tests.testPrintAllExtensions.__unittest_expecting_failure__ = True
text_format_test.Proto2Tests.testPrintAllExtensionsPointy.__unittest_expecting_failure__ = True
getattr(text_format_test.TextFormatMessageToStringTests, "testCustomOptions" + sep + "0").__unittest_expecting_failure__ = True
getattr(text_format_test.TextFormatMessageToStringTests, "testCustomOptions" + sep + "1").__unittest_expecting_failure__ = True
getattr(text_format_test.TextFormatMessageToStringTests, "testPrintUnknownFieldsEmbeddedMessageInBytes" + sep + "0").__unittest_expecting_failure__ = True
getattr(text_format_test.TextFormatMessageToStringTests, "testPrintUnknownFieldsEmbeddedMessageInBytes" + sep + "1").__unittest_expecting_failure__ = True

if __name__ == '__main__':
  unittest.main(module=text_format_test, verbosity=2)