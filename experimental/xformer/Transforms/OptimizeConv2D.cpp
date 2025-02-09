// Copyright 2023 XMOS LIMITED. This Software is subject to the terms of the
// XMOS Public License: Version 1

#include "IR/XCoreOps.h"
#include "Transforms/Options.h"

#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"

namespace mlir {
namespace xcore {

namespace {
// Optimize TFL Conv2D.
struct OptimizeConv2D
    : public PassWrapper<OptimizeConv2D, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(OptimizeConv2D)

  void getDependentDialects(DialectRegistry &registry) const final {
    registry.insert<TFL::TensorFlowLiteDialect>();
  }
  StringRef getArgument() const final { return "xcore-optimize-conv2d"; }
  StringRef getDescription() const final { return "Optimize TFL Conv2D."; }
  void runOnOperation() override;
};

struct ChannelwiseSplitConv2DOutputPattern
    : public OpRewritePattern<TFL::Conv2DOp> {
  using OpRewritePattern<TFL::Conv2DOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TFL::Conv2DOp op,
                                PatternRewriter &rewriter) const override {
    // Check for invalid types and return
    // Input type must be QI8
    auto inputElementType =
        op.getInput().getType().cast<ShapedType>().getElementType();
    if (!(inputElementType.isa<quant::QuantizedType>() &&
          inputElementType.cast<quant::QuantizedType>().isSigned() &&
          inputElementType.cast<quant::QuantizedType>()
                  .getStorageTypeIntegralWidth() == 8)) {
      return failure();
    }

    // Filter type must be
    auto filterElementType =
        op.getFilter().getType().cast<ShapedType>().getElementType();
    if (!(filterElementType.isa<quant::QuantizedType>() &&
          filterElementType.cast<quant::QuantizedType>().isSigned() &&
          filterElementType.cast<quant::QuantizedType>()
                  .getStorageTypeIntegralWidth() == 8)) {
      return failure();
    }

    // If bias exists, it must be QI32
    if (!op.getBias().getType().isa<NoneType>()) {
      auto biasElementType =
          op.getBias().getType().cast<ShapedType>().getElementType();

      if (!(biasElementType.isa<quant::QuantizedType>() &&
            biasElementType.cast<quant::QuantizedType>().isSigned() &&
            biasElementType.cast<quant::QuantizedType>()
                    .getStorageTypeIntegralWidth() == 32)) {
        return failure();
      }
    }

    // Lamdba to split filter or bias based on whether it is per channelwise
    // quantization.
    // If per channelwise quantization, we have to split the
    // quantization params.
    auto getSplitResultType = [](int splitSize, int elems,
                                 std::initializer_list<int64_t> &shape,
                                 TFL::QConstOp op, ShapedType opType) {
      Type splitResultType =
          RankedTensorType::get(shape, opType.getElementType());

      auto opQType = op.getQtype().template cast<RankedTensorType>();

      if (auto qType =
              opQType.getElementType()
                  .dyn_cast<mlir::quant::UniformQuantizedPerAxisType>()) {
        auto scaleVector =
            std::vector<double>{qType.getScales().begin() + elems,
                                qType.getScales().begin() + elems + splitSize};

        auto zeroPointVector = std::vector<int64_t>{
            qType.getZeroPoints().begin() + elems,
            qType.getZeroPoints().begin() + elems + splitSize};

        auto newQType = quant::UniformQuantizedPerAxisType::getChecked(
            op.getLoc(), qType.getFlags(), qType.getStorageType(),
            qType.getExpressedType(), scaleVector, zeroPointVector,
            qType.getQuantizedDimension(), qType.getStorageTypeMin(),
            qType.getStorageTypeMax());

        splitResultType = newQType.castFromExpressedType(
            quant::AnyQuantizedType::castToExpressedType(splitResultType));
      }
      return splitResultType;
    };

    auto filterQConstOp =
        dyn_cast<TFL::QConstOp>(op.getFilter().getDefiningOp());
    auto filterType = op.getFilter().getType().cast<ShapedType>();
    auto filter = filterQConstOp.getValue().cast<DenseElementsAttr>();
    auto filterSize = filter.size();

    // We want to try to keep the split filtersize less than specified size
    int numSplits = ceil(filterSize / convChannelwiseSplitSizeOption);
    // Only try to split if at least two splits are possible
    if (numSplits < 2) {
      return failure();
    }
    // Let's split the filter batch size as that's the same as bias size and
    // output channel size
    auto filterBatchSize = filterType.getShape()[0];
    // We want splits to be multiples of four, so we divide here and multiply
    // after calculating the split sizes
    int tmp = filterBatchSize / 4;
    int d = tmp / numSplits;
    int r = tmp % numSplits;
    llvm::SmallVector<int> splitSizes;
    // If not an even split, we distribute the remainder to the first few splits
    for (int i = 0; i < numSplits; ++i) {
      if (r > 0) {
        splitSizes.push_back((d + 1) * 4);
        r -= 1;
      } else {
        splitSizes.push_back(d * 4);
      }
    }

    bool biasPresent = true;
    if (!op.getBias().getType().isa<NoneType>()) {
      auto biasQConstOp = dyn_cast<TFL::QConstOp>(op.getBias().getDefiningOp());
      auto bias = biasQConstOp.getValue().cast<DenseElementsAttr>();
      assert(bias.size() == filterBatchSize);
    } else {
      biasPresent = false;
    }

    assert(op.getOutput().getType().cast<ShapedType>().getShape()[3] ==
           filterBatchSize);

    SmallVector<Value> conv2DOps;
    int elems = 0;
    for (int i = 0; i < splitSizes.size(); ++i) {
      // Create split filter
      auto splitFilterShape = {
          static_cast<int64_t>(splitSizes[i]), filterType.getShape()[1],
          filterType.getShape()[2], filterType.getShape()[3]};
      auto splitFilterResultType = getSplitResultType(
          splitSizes[i], elems, splitFilterShape, filterQConstOp, filterType);
      auto splitFilterValueType =
          RankedTensorType::get(splitFilterShape, rewriter.getIntegerType(8));
      auto filterSizeExcludingBatch = filterType.getShape()[1] *
                                      filterType.getShape()[2] *
                                      filterType.getShape()[3];
      auto filterVector = std::vector<int8_t>{
          filter.getValues<int8_t>().begin() + elems * filterSizeExcludingBatch,
          filter.getValues<int8_t>().begin() +
              ((elems + splitSizes[i]) * filterSizeExcludingBatch)};

      auto splitFilterQConstOp = rewriter.create<TFL::QConstOp>(
          op.getLoc(), mlir::TypeAttr::get(splitFilterResultType),
          mlir::DenseElementsAttr::get(splitFilterValueType,
                                       llvm::ArrayRef(filterVector)));

      Value splitBiasQConstOpOrNone;
      if (biasPresent) {
        // Create split bias
        auto biasQConstOp =
            dyn_cast<TFL::QConstOp>(op.getBias().getDefiningOp());
        auto biasType = op.getBias().getType().cast<ShapedType>();
        auto bias = biasQConstOp.getValue().cast<DenseElementsAttr>();

        auto splitBiasShape = {static_cast<int64_t>(splitSizes[i])};
        auto splitBiasResultType = getSplitResultType(
            splitSizes[i], elems, splitBiasShape, biasQConstOp, biasType);
        auto splitBiasValueType =
            RankedTensorType::get(splitBiasShape, rewriter.getIntegerType(32));
        auto biasVector = std::vector<int32_t>{
            bias.getValues<int32_t>().begin() + elems,
            bias.getValues<int32_t>().begin() + elems + splitSizes[i]};

        splitBiasQConstOpOrNone = rewriter.create<TFL::QConstOp>(
            op.getLoc(), mlir::TypeAttr::get(splitBiasResultType),
            mlir::DenseElementsAttr::get(splitBiasValueType,
                                         llvm::ArrayRef(biasVector)));
      } else {
        splitBiasQConstOpOrNone = op.getBias();
      }

      // Add split Conv2Ds
      auto outputType = op.getOutput().getType().cast<ShapedType>();
      auto splitResultType = RankedTensorType::get(
          {outputType.getShape()[0], outputType.getShape()[1],
           outputType.getShape()[2], splitSizes[i]},
          outputType.getElementType());

      auto newConv2DOp = rewriter.create<TFL::Conv2DOp>(
          op.getLoc(), splitResultType, op.getInput(), splitFilterQConstOp,
          splitBiasQConstOpOrNone, 1, 1, op.getFusedActivationFunction(),
          "VALID", 1, 1);

      conv2DOps.push_back(newConv2DOp.getResult());

      elems += splitSizes[i];
    }

    // Add concatenation op with axis 3, the channel dimension
    auto newConcatOp = rewriter.create<TFL::ConcatenationOp>(
        op.getLoc(), op.getOutput().getType(), conv2DOps, 3, "NONE");

    rewriter.replaceOp(op, newConcatOp.getOutput());
    return success();
  }
};

template <typename T>
Value createInputPadOp(int padDepthSize, T convOp, PatternRewriter &rewriter) {
  RankedTensorType paddingsType =
      RankedTensorType::get({4, 2}, rewriter.getI32Type());

  // Pad the input depth
  std::vector<int32_t> paddingsValues = {0, 0, 0, 0, 0, 0, 0, padDepthSize};
  Value paddings = rewriter.create<TFL::ConstOp>(
      convOp.getLoc(), DenseIntElementsAttr::get(paddingsType, paddingsValues));
  auto inputShape =
      convOp.getInput().getType().template cast<RankedTensorType>().getShape();

  auto paddedInputResultType = RankedTensorType::get(
      {inputShape[0], inputShape[1], inputShape[2],
       inputShape[3] + padDepthSize},
      convOp.getInput().getType().template cast<ShapedType>().getElementType());

  // Set the PadOp output as the Conv2D input
  Value padOpOutput = rewriter.create<TFL::PadOp>(
      convOp.getLoc(), paddedInputResultType, convOp.getInput(), paddings);
  return padOpOutput;
}

Type getPaddedResultType(int padSize, std::initializer_list<int64_t> &shape,
                         TFL::QConstOp op, ShapedType opType) {
  Type paddedResultType = RankedTensorType::get(shape, opType.getElementType());

  auto opQType = op.getQtype().template cast<RankedTensorType>();
  if (auto qType = opQType.getElementType()
                       .dyn_cast<mlir::quant::UniformQuantizedPerAxisType>()) {
    auto scaleVector =
        std::vector<double>{qType.getScales().begin(), qType.getScales().end()};
    llvm::SmallVector<double, 0> paddedScaleVector;
    paddedScaleVector.reserve(scaleVector.size() + padSize);
    paddedScaleVector.insert(paddedScaleVector.end(), scaleVector.begin(),
                             scaleVector.end());
    paddedScaleVector.insert(paddedScaleVector.end(), padSize, 1.0);

    auto zeroPointVector = std::vector<int64_t>{qType.getZeroPoints().begin(),
                                                qType.getZeroPoints().end()};
    llvm::SmallVector<int64_t, 0> paddedZeroPointVector;
    paddedZeroPointVector.reserve(zeroPointVector.size() + padSize);
    paddedZeroPointVector.insert(paddedZeroPointVector.end(),
                                 zeroPointVector.begin(),
                                 zeroPointVector.end());
    paddedZeroPointVector.insert(paddedZeroPointVector.end(), padSize, 0);

    auto newQType = quant::UniformQuantizedPerAxisType::getChecked(
        op.getLoc(), qType.getFlags(), qType.getStorageType(),
        qType.getExpressedType(), paddedScaleVector, paddedZeroPointVector,
        qType.getQuantizedDimension(), qType.getStorageTypeMin(),
        qType.getStorageTypeMax());

    paddedResultType = newQType.castFromExpressedType(
        quant::AnyQuantizedType::castToExpressedType(paddedResultType));
  }

  return paddedResultType;
}

template <typename T>
Value createPaddedFilterOp(int padSize, int padDim, T convOp,
                           PatternRewriter &rewriter) {
  assert(padDim == 0 || padDim == 3 && "filter padDim must be 0 or 3!");
  auto filterQConstOp =
      dyn_cast<TFL::QConstOp>(convOp.getFilter().getDefiningOp());
  auto filter = filterQConstOp.getValue().template cast<DenseElementsAttr>();

  auto filterVector =
      std::vector<int8_t>{filter.template getValues<int8_t>().begin(),
                          filter.template getValues<int8_t>().end()};
  auto filterShape =
      convOp.getFilter().getType().template cast<RankedTensorType>().getShape();
  std::vector<int64_t> paddedShape(4, 0);
  for (int i = 0; i < 4; i++) {
    paddedShape[i] = filterShape[i];
    if (i == padDim) {
      paddedShape[i] += padSize;
    }
  }

  llvm::SmallVector<int8_t, 0> paddedFilterVector;
  paddedFilterVector.reserve(paddedShape[0] * paddedShape[1] * paddedShape[2] *
                             paddedShape[3]);

  if (padDim == 3) {
    // Pad the filter depth
    for (int i = 0; i < filterVector.size(); i += filterShape[3]) {
      paddedFilterVector.insert(paddedFilterVector.end(),
                                filterVector.begin() + i,
                                filterVector.begin() + i + filterShape[3]);
      paddedFilterVector.insert(paddedFilterVector.end(), padSize, 0);
    }
  } else {
    // Pad the filter batch
    paddedFilterVector.insert(paddedFilterVector.end(), filterVector.begin(),
                              filterVector.end());
    paddedFilterVector.insert(
        paddedFilterVector.end(),
        padSize * paddedShape[1] * paddedShape[2] * paddedShape[3], 0);
  }

  auto paddedFilterShape = {paddedShape[0], paddedShape[1], paddedShape[2],
                            paddedShape[3]};
  Type paddedFilterResultType;
  if (convOp.GetQuantizationDimIndex() == padDim) {
    // If the pad dimension is the quantization dimension, then might have
    // to pad the quantization params
    paddedFilterResultType = getPaddedResultType(
        padSize, paddedFilterShape, filterQConstOp,
        convOp.getFilter().getType().template cast<ShapedType>());
  } else {
    paddedFilterResultType = RankedTensorType::get(
        paddedFilterShape, convOp.getFilter()
                               .getType()
                               .template cast<ShapedType>()
                               .getElementType());
  }

  RankedTensorType paddedFilterValueType =
      RankedTensorType::get(paddedFilterShape, rewriter.getIntegerType(8));

  // Create a new QConstOp with the padded data
  Value paddedFilterOp = rewriter.create<TFL::QConstOp>(
      convOp.getLoc(), mlir::TypeAttr::get(paddedFilterResultType),
      DenseElementsAttr::get<int8_t>(paddedFilterValueType,
                                     paddedFilterVector));
  return paddedFilterOp;
}

template <typename T>
Value createPaddedBiasOp(int padSize, T convOp, PatternRewriter &rewriter) {
  auto biasQConstOp = dyn_cast<TFL::QConstOp>(convOp.getBias().getDefiningOp());
  auto bias = biasQConstOp.getValue().template cast<DenseElementsAttr>();

  std::vector<int32_t> biasVector;
  DenseElementsAttr biasesAttr;
  if (convOp.getBias()
          .getType()
          .template cast<ShapedType>()
          .getElementType()
          .template isa<quant::QuantizedType>()) {
    auto biasQConstOp =
        dyn_cast<TFL::QConstOp>(convOp.getBias().getDefiningOp());
    biasesAttr = biasQConstOp.getValue().template cast<DenseElementsAttr>();
  } else {
    matchPattern(convOp.getBias(), m_Constant(&biasesAttr));
  }
  biasVector =
      std::vector<int32_t>{biasesAttr.template getValues<int32_t>().begin(),
                           biasesAttr.template getValues<int32_t>().end()};

  auto biasShape =
      convOp.getBias().getType().template cast<RankedTensorType>().getShape();

  llvm::SmallVector<int32_t, 0> paddedBiasVector;
  paddedBiasVector.reserve(biasShape[0] + padSize);

  // Pad the bias batch
  paddedBiasVector.insert(paddedBiasVector.end(), biasVector.begin(),
                          biasVector.end());
  paddedBiasVector.insert(paddedBiasVector.end(), padSize, 0);

  auto paddedBiasShape = {biasShape[0] + padSize};
  auto paddedBiasResultType = getPaddedResultType(
      padSize, paddedBiasShape, biasQConstOp,
      convOp.getBias().getType().template cast<ShapedType>());
  RankedTensorType paddedBiasValueType =
      RankedTensorType::get(paddedBiasShape, rewriter.getIntegerType(32));

  // Create a new QConstOp with the padded data
  Value paddedBiasOp = rewriter.create<TFL::QConstOp>(
      convOp.getLoc(), mlir::TypeAttr::get(paddedBiasResultType),
      DenseElementsAttr::get<int32_t>(paddedBiasValueType, paddedBiasVector));
  return paddedBiasOp;
}

template <typename T>
TFL::StridedSliceOp
createPaddedConvWithStridedSliceOp(int padSize, T convOp, Value paddedFilterOp,
                                   Value paddedBiasOp,
                                   PatternRewriter &rewriter) {
  auto outputShape =
      convOp.getOutput().getType().template cast<RankedTensorType>().getShape();
  auto convReplacement = llvm::cast<T>(rewriter.clone(*convOp));

  RankedTensorType newConvType =
      RankedTensorType::get({outputShape[0], outputShape[1], outputShape[2],
                             outputShape[3] + padSize},
                            convOp.getOutput()
                                .getType()
                                .template cast<ShapedType>()
                                .getElementType());
  if (!convOp.getBias().getType().template isa<NoneType>()) {
    convReplacement.setOperand(2, paddedBiasOp);
  }
  convReplacement->getResult(0).setType(newConvType);
  convReplacement.setOperand(1, paddedFilterOp);

  // Create strided slice op
  int stridesAttr[4] = {1, 1, 1, 1};
  auto stridesConstantOp = rewriter.create<arith::ConstantOp>(
      convReplacement.getLoc(), rewriter.getI32TensorAttr(stridesAttr));

  int beginAttr[4] = {0, 0, 0, 0};
  auto beginConstantOp = rewriter.create<arith::ConstantOp>(
      convReplacement.getLoc(), rewriter.getI32TensorAttr(beginAttr));

  int endAttr[4] = {static_cast<int32_t>(1),
                    static_cast<int32_t>(outputShape[1]),
                    static_cast<int32_t>(outputShape[2]),
                    static_cast<int32_t>(outputShape[3])};
  auto endConstantOp = rewriter.create<arith::ConstantOp>(
      convReplacement.getLoc(), rewriter.getI32TensorAttr(endAttr));

  auto stridedSliceOp = rewriter.create<TFL::StridedSliceOp>(
      convOp.getLoc(), convOp.getOutput().getType(), convReplacement,
      beginConstantOp, endConstantOp, stridesConstantOp, 0, 0, 0, 0, 0);
  return stridedSliceOp;
}

struct PadTo4Conv2DInputPattern : public OpRewritePattern<TFL::Conv2DOp> {
  using OpRewritePattern<TFL::Conv2DOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TFL::Conv2DOp conv2DOp,
                                PatternRewriter &rewriter) const override {
    // Check for invalid types and return
    // Input type must be QI8
    auto inputElementType =
        conv2DOp.getInput().getType().cast<ShapedType>().getElementType();
    if (!(inputElementType.isa<quant::QuantizedType>() &&
          inputElementType.cast<quant::QuantizedType>().isSigned() &&
          inputElementType.cast<quant::QuantizedType>()
                  .getStorageTypeIntegralWidth() == 8)) {
      return failure();
    }

    // Filter type must be
    auto filterElementType =
        conv2DOp.getFilter().getType().cast<ShapedType>().getElementType();
    if (!(filterElementType.isa<quant::QuantizedType>() &&
          filterElementType.cast<quant::QuantizedType>().isSigned() &&
          filterElementType.cast<quant::QuantizedType>()
                  .getStorageTypeIntegralWidth() == 8)) {
      return failure();
    }

    // If bias exists, it must be QI32
    if (!conv2DOp.getBias().getType().isa<NoneType>()) {
      auto biasElementType =
          conv2DOp.getBias().getType().cast<ShapedType>().getElementType();

      if (!(biasElementType.isa<quant::QuantizedType>() &&
            biasElementType.cast<quant::QuantizedType>().isSigned() &&
            biasElementType.cast<quant::QuantizedType>()
                    .getStorageTypeIntegralWidth() == 32)) {
        return failure();
      }
    }

    // Align depth up to multiple of four
    auto inputDepth =
        conv2DOp.getInput().getType().cast<ShapedType>().getDimSize(3);
    int padDepthSize = (((inputDepth + 3) / 4) * 4) - inputDepth;

    if (padDepthSize == 0) {
      return failure();
    }

    // Pad the Conv2D input
    Value padOpOutput = createInputPadOp(padDepthSize, conv2DOp, rewriter);
    conv2DOp.setOperand(0, padOpOutput);

    // Pad the Conv2D filter
    // We need to do this at compile time instead of using a PadOp
    // as we use the padded filter values for the boggling calculations
    // for creating the XC Conv2D ops
    Value paddedQConstOpOutput =
        createPaddedFilterOp(padDepthSize, /*padDim=*/3, conv2DOp, rewriter);
    // Set the QConstOp output as the Conv2D filter
    conv2DOp.setOperand(1, paddedQConstOpOutput);

    return success();
  }
};

struct PadTo4Conv2DOutputPattern : public OpRewritePattern<TFL::Conv2DOp> {
  using OpRewritePattern<TFL::Conv2DOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TFL::Conv2DOp conv2DOp,
                                PatternRewriter &rewriter) const override {
    // Check for invalid types and return
    // Input type must be QI8
    auto inputElementType =
        conv2DOp.getInput().getType().cast<ShapedType>().getElementType();
    if (!(inputElementType.isa<quant::QuantizedType>() &&
          inputElementType.cast<quant::QuantizedType>().isSigned() &&
          inputElementType.cast<quant::QuantizedType>()
                  .getStorageTypeIntegralWidth() == 8)) {
      return failure();
    }

    // Filter type must be
    auto filterElementType =
        conv2DOp.getFilter().getType().cast<ShapedType>().getElementType();
    if (!(filterElementType.isa<quant::QuantizedType>() &&
          filterElementType.cast<quant::QuantizedType>().isSigned() &&
          filterElementType.cast<quant::QuantizedType>()
                  .getStorageTypeIntegralWidth() == 8)) {
      return failure();
    }

    // If bias exists, it must be QI32
    if (!conv2DOp.getBias().getType().isa<NoneType>()) {
      auto biasElementType =
          conv2DOp.getBias().getType().cast<ShapedType>().getElementType();

      if (!(biasElementType.isa<quant::QuantizedType>() &&
            biasElementType.cast<quant::QuantizedType>().isSigned() &&
            biasElementType.cast<quant::QuantizedType>()
                    .getStorageTypeIntegralWidth() == 32)) {
        return failure();
      }
    }

    // Align depth up to multiple of four
    auto outputShape = conv2DOp.getOutput()
                           .getType()
                           .template cast<RankedTensorType>()
                           .getShape();
    int padSize = (((outputShape[3] + 3) / 4) * 4) - outputShape[3];

    if (padSize == 0) {
      return failure();
    }

    // Pad the Conv2D filter
    // We need to do this at compile time instead of using a PadOp
    // as we use the padded filter values for the boggling calculations
    // for creating the XC Conv2D ops
    Value paddedFilterOp =
        createPaddedFilterOp(padSize, /*padDim=*/0, conv2DOp, rewriter);
    Value paddedBiasOp;
    if (!conv2DOp.getBias().getType().template isa<NoneType>()) {
      paddedBiasOp = createPaddedBiasOp(padSize, conv2DOp, rewriter);
    }

    // Create conv op with padded output size and Strided Slice to slice the
    // padded output
    auto stridedSliceOp = createPaddedConvWithStridedSliceOp(
        padSize, conv2DOp, paddedFilterOp, paddedBiasOp, rewriter);

    // Replace op with strided slice op
    rewriter.replaceOp(conv2DOp, stridedSliceOp.getOutput());

    return success();
  }
};

struct PadTo4DepthwiseConv2DPattern
    : public OpRewritePattern<TFL::DepthwiseConv2DOp> {
  using OpRewritePattern<TFL::DepthwiseConv2DOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TFL::DepthwiseConv2DOp dConv2DOp,
                                PatternRewriter &rewriter) const override {
    // Check for invalid types and return
    // Input type must be QI8
    auto inputElementType =
        dConv2DOp.getInput().getType().cast<ShapedType>().getElementType();
    if (!(inputElementType.isa<quant::QuantizedType>() &&
          inputElementType.cast<quant::QuantizedType>().isSigned() &&
          inputElementType.cast<quant::QuantizedType>()
                  .getStorageTypeIntegralWidth() == 8)) {
      return failure();
    }

    // Filter type must be
    auto filterElementType =
        dConv2DOp.getFilter().getType().cast<ShapedType>().getElementType();
    if (!(filterElementType.isa<quant::QuantizedType>() &&
          filterElementType.cast<quant::QuantizedType>().isSigned() &&
          filterElementType.cast<quant::QuantizedType>()
                  .getStorageTypeIntegralWidth() == 8)) {
      return failure();
    }

    // If bias exists, it must be QI32
    if (!dConv2DOp.getBias().getType().isa<NoneType>()) {
      auto biasElementType =
          dConv2DOp.getBias().getType().cast<ShapedType>().getElementType();

      if (!(biasElementType.isa<quant::QuantizedType>() &&
            biasElementType.cast<quant::QuantizedType>().isSigned() &&
            biasElementType.cast<quant::QuantizedType>()
                    .getStorageTypeIntegralWidth() == 32)) {
        return failure();
      }
    }

    // Align depth up to multiple of four
    auto inputDepth =
        dConv2DOp.getInput().getType().cast<ShapedType>().getDimSize(3);
    auto outputShape = dConv2DOp.getOutput()
                           .getType()
                           .template cast<RankedTensorType>()
                           .getShape();
    if (inputDepth != outputShape[3]) {
      return failure();
    }
    int padSize = (((inputDepth + 3) / 4) * 4) - inputDepth;

    if (padSize == 0) {
      return failure();
    }

    // Pad the dConv2D input
    Value padOpOutput = createInputPadOp(padSize, dConv2DOp, rewriter);
    dConv2DOp.setOperand(0, padOpOutput);

    // Pad the dConv2D filter
    Value paddedFilterOp =
        createPaddedFilterOp(padSize, /*padDim=*/3, dConv2DOp, rewriter);
    Value paddedBiasOp;
    if (!dConv2DOp.getBias().getType().template isa<NoneType>()) {
      paddedBiasOp = createPaddedBiasOp(padSize, dConv2DOp, rewriter);
    }

    // Create conv op with padded output size and Strided Slice to slice the
    // padded output
    auto stridedSliceOp = createPaddedConvWithStridedSliceOp(
        padSize, dConv2DOp, paddedFilterOp, paddedBiasOp, rewriter);

    // Replace op with strided slice op
    rewriter.replaceOp(dConv2DOp, stridedSliceOp.getOutput());

    return success();
  }
};

void OptimizeConv2D::runOnOperation() {
  auto *ctx = &getContext();
  func::FuncOp func = getOperation();
  RewritePatternSet patterns(ctx);

  // To align Conv2D input to 4 channels, we insert a pad op to pad the input
  // channels and pad the conv filter channels
  patterns.insert<PadTo4Conv2DInputPattern>(ctx);
  // To align Conv2D output to 4 channels, we pad the conv filter batch and
  // bias, pad conv2d output channels, and add a strided slice to remove the
  // padded section
  patterns.insert<PadTo4Conv2DOutputPattern>(ctx);
  // For DepthwiseConv2D, input and output channels are the same.
  // To align DepthwiseConv2D input/output to 4 channels, we insert a pad op to
  // pad the input channels, pad the conv filter channels and bias, pad
  // conv2d output channels, and add a strided slice to remove the padded
  // section
  patterns.insert<PadTo4DepthwiseConv2DPattern>(ctx);
  // When the filter is too large, we channelwise split the conv2d output to
  // make multiple conv2ds so that the filter for each can be loaded separately.
  // This means the filter batch gets split. We also have to split the
  // quantization params.
  patterns.insert<ChannelwiseSplitConv2DOutputPattern>(ctx);
  (void)applyPatternsAndFoldGreedily(func, std::move(patterns));
}
} // namespace

// Creates an instance of the OptimizeConv2D pass.
std::unique_ptr<OperationPass<func::FuncOp>> createOptimizeConv2DPass() {
  return std::make_unique<OptimizeConv2D>();
}

static PassRegistration<OptimizeConv2D> pass;

} // namespace xcore
} // namespace mlir
