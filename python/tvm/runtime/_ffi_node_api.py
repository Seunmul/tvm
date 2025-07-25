# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# pylint: disable=invalid-name, unused-argument
"""FFI for tvm.node"""
import tvm.ffi
import tvm.ffi.core


# The implementations below are default ones when the corresponding
# functions are not available in the runtime only mode.
# They will be overriden via _init_api to the ones registered
def AsRepr(obj):
    return type(obj).__name__ + "(" + obj.__ctypes_handle__().value + ")"


def NodeListAttrNames(obj):
    return lambda x: 0


def NodeGetAttr(obj, name):
    raise AttributeError()


def SaveJSON(obj):
    raise RuntimeError("Do not support object serialization in runtime only mode")


def LoadJSON(json_str):
    raise RuntimeError("Do not support object serialization in runtime only mode")


# Exports functions registered in node namespace.
tvm.ffi._init_api("node", __name__)
