# Copyright (c) 2019, XMOS Ltd, All rights reserved

import os
import pytest
from pathlib import Path

import numpy as np

from tflite2xcore.xcore_interpreter import XCOREInterpreter

BUILTIN_OPERATORS_TEST_MODEL = os.path.join(
    Path(__file__).parent.absolute(),
    'builtin_operators.tflite'
)

BUILTIN_OPERATORS_TEST_INPUT = os.path.join(
    Path(__file__).parent.absolute(),
    'test_0.x'
)

BUILTIN_OPERATORS_TEST_OUTPUT = os.path.join(
    Path(__file__).parent.absolute(),
    'test_0.y'
)


def test_allocate_tensors():
    with open(BUILTIN_OPERATORS_TEST_MODEL, 'rb') as fd:
        model_content = fd.read()
    interpreter = XCOREInterpreter(model_content=model_content, arena_size=30000)
    interpreter.allocate_tensors()
    assert(interpreter)


def test_model_content():
    with open(BUILTIN_OPERATORS_TEST_MODEL, 'rb') as fd:
        model_content = fd.read()
    interpreter = XCOREInterpreter(model_content=model_content)
    assert(interpreter)


def test_model_path():
    interpreter = XCOREInterpreter(model_path=BUILTIN_OPERATORS_TEST_MODEL)
    assert(interpreter)


def test_inference():
    with open(BUILTIN_OPERATORS_TEST_MODEL, 'rb') as fd:
        model_content = fd.read()
    interpreter = XCOREInterpreter(model_content=model_content)

    interpreter.allocate_tensors()

    input_tensor_details = interpreter.get_input_details()[0]
    output_tensor_details = interpreter.get_output_details()[0]

    input_tensor = np.fromfile(BUILTIN_OPERATORS_TEST_INPUT, dtype=input_tensor_details['dtype'])
    input_tensor.shape = input_tensor_details['shape']

    expected_output = np.fromfile(BUILTIN_OPERATORS_TEST_OUTPUT, dtype=output_tensor_details['dtype'])
    expected_output.shape = output_tensor_details['shape']

    interpreter.set_tensor(input_tensor_details['index'], input_tensor)
    interpreter.invoke()
    computed_output = interpreter.get_tensor(output_tensor_details['index'])

    np.testing.assert_equal(computed_output, expected_output)

if __name__ == "__main__":
    pytest.main()
