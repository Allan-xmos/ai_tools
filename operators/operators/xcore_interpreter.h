// Copyright (c) 2020, XMOS Ltd, All rights reserved
#ifndef XCORE_INTERPRETER_H_
#define XCORE_INTERPRETER_H_

#include "operators/dispatcher.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"

namespace xcore {

class XCoreInterpreter : public tflite::MicroInterpreter {
 public:
  XCoreInterpreter(const tflite::Model* model,
                   const tflite::MicroOpResolver& resolver, uint8_t* arena,
                   size_t arena_size, tflite::ErrorReporter* reporter,
                   bool use_curent_thread = true,
                   tflite::Profiler* profiler = nullptr);

 private:
  Dispatcher dispatcher_;
};
}  // namespace xcore

#endif  // XCORE_INTERPRETER_H_