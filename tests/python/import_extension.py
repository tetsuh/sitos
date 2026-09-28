# Copyright 2026 sitos contributors
# SPDX-License-Identifier: Apache-2.0

import importlib
import sys

sys.path.insert(0, sys.argv[1])
module = importlib.import_module("_sitos")
assert hasattr(module, "BufferPublisher")
assert hasattr(module, "BufferClass")
assert hasattr(module, "FenceDurability")
assert hasattr(module, "FenceReceipt")
