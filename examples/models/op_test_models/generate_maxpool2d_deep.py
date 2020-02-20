#!/usr/bin/env python
#
# Copyright (c) 2018-2019, XMOS Ltd, All rights reserved
import argparse
from pathlib import Path
from tflite2xcore.model_generation import utils
from tflite2xcore.model_generation.interface import KerasModel
import tensorflow as tf
import op_test_models_common as common

DEFAULT_INPUTS = 32
DEFAULT_HEIGHT = 4
DEFAULT_WIDTH = DEFAULT_HEIGHT

DEFAULT_POOL_SIZE = 2
DEFAULT_PADDING = "valid"
DEFAULT_STRIDES = 2
DEFAULT_PATH = Path(__file__).parent.joinpath("debug", "maxpool_2d_deep").resolve()


class MaxPool2d(common.DefaultOpTestModel):
    def build(self, height, width, input_channels, pool, stride, pad):
        assert input_channels % 32 == 0, "# of input channels must be multiple of 32"
        assert height % 2 == 0, "height must be even"
        assert width % 2 == 0, "width must be even"
        assert pool == 2, "pool size must be 2"
        assert stride == 2, "stride must be 2"
        assert pad.lower() == "valid", "padding mode must be valid"
        super().build()

        # Building
        self.core_model = tf.keras.Sequential(
            name=self.name,
            layers=[
                tf.keras.layers.MaxPool2D(
                    pool_size=pool,
                    strides=stride,
                    padding=pad,
                    input_shape=(height, width, input_channels),
                )
            ],
        )
        # Compilation
        self.core_model.compile(
            optimizer="adam",
            loss="sparse_categorical_crossentropy",
            metrics=["accuracy"],
        )
        # Show summary
        self.core_model.summary()


def main(
    path=DEFAULT_PATH,
    *,
    input_channels=DEFAULT_INPUTS,
    height=DEFAULT_HEIGHT,
    width=DEFAULT_WIDTH,
    pool_size=DEFAULT_POOL_SIZE,
    padding=DEFAULT_PADDING,
    strides=DEFAULT_STRIDES
):
    # Instantiate model
    test_model = MaxPool2d("maxpool2d_deep", Path(path))
    # Build model and compile
    test_model.build(height, width, input_channels, pool_size, strides, padding)
    test_model.run()


if __name__ == "__main__":
    parser = common.OpTestDimParser(
        defaults={
            "path": DEFAULT_PATH,
            "inputs": DEFAULT_INPUTS,
            "width": DEFAULT_WIDTH,
            "height": DEFAULT_HEIGHT,
            "padding": DEFAULT_PADDING,
        }
    )
    parser.add_argument(
        "-st", "--strides", type=int, default=DEFAULT_STRIDES, help="Stride"
    )
    parser.add_argument(
        "-po", "--pool_size", type=int, default=DEFAULT_POOL_SIZE, help="Pool size"
    )
    args = parser.parse_args()

    utils.set_verbosity(args.verbose)
    utils.set_gpu_usage(False, args.verbose)

    main(
        path=args.path,
        input_channels=args.inputs,
        height=args.height,
        width=args.width,
        pool_size=args.pool_size,
        padding=args.padding,
        strides=args.strides,
    )
