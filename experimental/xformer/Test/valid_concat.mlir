// RUN: xcore-opt --mlir-io --xcore-replace-concat %s | FileCheck %s 

// CHECK-LABEL: main
func @main(%arg0: tensor<1x6x6x4x!quant.uniform<i8:f32, 0.0039214449934661388:-128>>) -> tensor<1x144x!quant.uniform<i8:f32, 0.0032160764094442129:-128>> attributes {tf.entry_function = {inputs = "serving_default_input_2:0", outputs = "StatefulPartitionedCall:0"}} {
  
  // CHECK: xc.concat
  // CHECK-NOT: tfl.concatenation

  %cst = constant dense<[-1, 144]> : tensor<2xi32>
  %0 = "xc.strided_slice"(%arg0) {begin_x = 0 : i32, begin_y = 0 : i32, memcpy_fn_param = "\18\00\00\00\04\00\00\00\05\00\00\00\05\00\00\00\00\00\00\00\1C\00\00\00\E4\FF\FF\FF\00\00\00\00"} : (tensor<1x6x6x4x!quant.uniform<i8:f32, 0.0039214449934661388:-128>>) -> tensor<1x6x3x4x!quant.uniform<i8:f32, 0.0039214449934661388:-128>>
  %1 = "xc.strided_slice"(%arg0) {begin_x = 6 : i32, begin_y = 0 : i32, memcpy_fn_param = "\18\00\00\00\04\00\00\00\05\00\00\00\02\00\00\00\00\00\00\00\1C\00\00\00\E4\FF\FF\FF\0C\00\00\00"} : (tensor<1x6x6x4x!quant.uniform<i8:f32, 0.0039214449934661388:-128>>) -> tensor<1x6x3x4x!quant.uniform<i8:f32, 0.0039214449934661388:-128>>
  %2 = "tfl.pseudo_qconst"() {qtype = tensor<4x1x1x4x!quant.uniform<i8<-127:127>:f32:0, {0.0057157878763973713,0.0063750739209353924,0.0039762118831276894,0.0061128144152462482}>>, value = dense<[[[[-17, 25, -70, -127]]], [[[24, 51, 11, -127]]], [[[-127, 22, -2, -44]]], [[[127, -84, 14, -67]]]]> : tensor<4x1x1x4xi8>} : () -> tensor<4x1x1x4x!quant.uniform<i8<-127:127>:f32:0, {0.0057157878763973713,0.0063750739209353924,0.0039762118831276894,0.0061128144152462482}>>
  %3 = "tfl.pseudo_qconst"() {qtype = tensor<4x!quant.uniform<i32:f32:0, {2.2414147679228336E-5,2.4999500965350308E-5,1.5592495401506312E-5,2.3971066184458323E-5}>>, value = dense<0> : tensor<4xi32>} : () -> tensor<4x!quant.uniform<i32:f32:0, {2.2414147679228336E-5,2.4999500965350308E-5,1.5592495401506312E-5,2.3971066184458323E-5}>>
  %4 = "tfl.conv_2d"(%0, %2, %3) {dilation_h_factor = 1 : i32, dilation_w_factor = 1 : i32, fused_activation_function = "RELU", padding = "VALID", stride_h = 1 : i32, stride_w = 1 : i32} : (tensor<1x6x3x4x!quant.uniform<i8:f32, 0.0039214449934661388:-128>>, tensor<4x1x1x4x!quant.uniform<i8<-127:127>:f32:0, {0.0057157878763973713,0.0063750739209353924,0.0039762118831276894,0.0061128144152462482}>>, tensor<4x!quant.uniform<i32:f32:0, {2.2414147679228336E-5,2.4999500965350308E-5,1.5592495401506312E-5,2.3971066184458323E-5}>>) -> tensor<1x6x3x4x!quant.uniform<i8:f32, 0.0032160764094442129:-128>>
  %5 = "tfl.pseudo_qconst"() {qtype = tensor<4x1x1x4x!quant.uniform<i8<-127:127>:f32:0, {0.0057157878763973713,0.0063750739209353924,0.0039762118831276894,0.0061128144152462482}>>, value = dense<[[[[-17, 25, -70, -127]]], [[[24, 51, 11, -127]]], [[[-127, 22, -2, -44]]], [[[127, -84, 14, -67]]]]> : tensor<4x1x1x4xi8>} : () -> tensor<4x1x1x4x!quant.uniform<i8<-127:127>:f32:0, {0.0057157878763973713,0.0063750739209353924,0.0039762118831276894,0.0061128144152462482}>>
  %6 = "tfl.pseudo_qconst"() {qtype = tensor<4x!quant.uniform<i32:f32:0, {2.2414147679228336E-5,2.4999500965350308E-5,1.5592495401506312E-5,2.3971066184458323E-5}>>, value = dense<0> : tensor<4xi32>} : () -> tensor<4x!quant.uniform<i32:f32:0, {2.2414147679228336E-5,2.4999500965350308E-5,1.5592495401506312E-5,2.3971066184458323E-5}>>
  %7 = "tfl.conv_2d"(%1, %5, %6) {dilation_h_factor = 1 : i32, dilation_w_factor = 1 : i32, fused_activation_function = "RELU", padding = "VALID", stride_h = 1 : i32, stride_w = 1 : i32} : (tensor<1x6x3x4x!quant.uniform<i8:f32, 0.0039214449934661388:-128>>, tensor<4x1x1x4x!quant.uniform<i8<-127:127>:f32:0, {0.0057157878763973713,0.0063750739209353924,0.0039762118831276894,0.0061128144152462482}>>, tensor<4x!quant.uniform<i32:f32:0, {2.2414147679228336E-5,2.4999500965350308E-5,1.5592495401506312E-5,2.3971066184458323E-5}>>) -> tensor<1x6x3x4x!quant.uniform<i8:f32, 0.0032160764094442129:-128>>
  %8 = "tfl.concatenation"(%4, %7) {axis = 2 : i32, fused_activation_function = "NONE"} : (tensor<1x6x3x4x!quant.uniform<i8:f32, 0.0032160764094442129:-128>>, tensor<1x6x3x4x!quant.uniform<i8:f32, 0.0032160764094442129:-128>>) -> tensor<1x6x6x4x!quant.uniform<i8:f32, 0.0032160764094442129:-128>>
  %9 = "tfl.reshape"(%8, %cst) : (tensor<1x6x6x4x!quant.uniform<i8:f32, 0.0032160764094442129:-128>>, tensor<2xi32>) -> tensor<1x144x!quant.uniform<i8:f32, 0.0032160764094442129:-128>>
  // CHECK: return
  return %9 : tensor<1x144x!quant.uniform<i8:f32, 0.0032160764094442129:-128>>
}
