# Copyright (c) 2020, XMOS Ltd, All rights reserved

import pytest  # type: ignore

from .test_depthwise_conv2d import converted_op_code, DepthwiseConv2dTestModelGenerator
from . import (
    ExplicitPaddingMixin,
    test_output,
    test_converted_single_op_model,
    test_idempotence,
)


#  ----------------------------------------------------------------------------
#                                   GENERATORS
#  ----------------------------------------------------------------------------


class PaddedDepthwiseConv2dTestModelGenerator(
    ExplicitPaddingMixin, DepthwiseConv2dTestModelGenerator
):
    pass


GENERATOR = PaddedDepthwiseConv2dTestModelGenerator


if __name__ == "__main__":
    pytest.main()
