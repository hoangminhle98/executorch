/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Generate tokens in a loop.
#pragma once

#include <executorch/extension/llm/runner/stats.h>
#include <executorch/extension/llm/runner/text_decoder_runner.h>
#include <executorch/extension/llm/tokenizer/tokenizer.h>

namespace torch::executor {
using Stats = ::executorch::llm::Stats;

class TextTokenGenerator {
 public:
  TextTokenGenerator(
      Tokenizer* tokenizer,
      TextDecoderRunner* text_decoder_runner,
      bool use_kv_cache,
      uint64_t eos_id,
      Stats* stats)
      : tokenizer_(tokenizer),
        text_decoder_runner_(text_decoder_runner),
        eos_id_(eos_id),
        use_kv_cache_(use_kv_cache),
        stats_(stats) {}

  /**
   * Token generation loop.
   * @param tokens prompt tokens as well as the first token generated by
   * prefill.
   * @param start_pos the start position of the new tokens, based on how many
   * prompt tokens is prefilled.
   * @param seq_len the total sequence length, including the prompt tokens, next
   * token from prefill and new tokens.
   * @param token_callback what to do after a token is generated.
   * @return how many tokens are generated.
   */
  inline Result<int64_t> generate(
      std::vector<uint64_t> tokens,
      int64_t start_pos,
      int32_t seq_len,
      std::function<void(const std::string&)> token_callback) {
    ET_CHECK_MSG(
        !tokens.empty(), "Token generation loop shouldn't take empty tokens");
    int64_t pos = start_pos; // position in the sequence

    std::vector<uint64_t> token_data; // allocate space for the tokens
    std::vector<exec_aten::SizesType> token_shape;

    // Token after prefill
    uint64_t cur_token = tokens.back();
    uint64_t prev_token;

    if (use_kv_cache_) {
      // hard code these to size 1 as kv cache is locked to static size right
      // now.
      token_data = {cur_token};
      token_shape = {1, 1};
    } else {
      token_data = tokens;
      token_shape = {1, static_cast<int>(tokens.size())};
    }

    // initialize tensor wrappers
    ManagedTensor tokens_managed(
        token_data.data(), token_shape, ScalarType::Long);

    ManagedTensor start_pos_managed(&pos, {1}, ScalarType::Long);

    // Generate our tokens
    while (pos < seq_len) {
      // Run the model
      Result<exec_aten::Tensor> logits_res =
          text_decoder_runner_->step(tokens_managed, start_pos_managed);

      ET_CHECK_OK_OR_RETURN_ERROR(logits_res.error());
      exec_aten::Tensor& logits_tensor = logits_res.get();

      prev_token = cur_token;

      stats_->on_sampling_begin();
      cur_token = text_decoder_runner_->logits_to_token(logits_tensor);
      stats_->on_sampling_end();

      pos++;

      if (use_kv_cache_) {
        // update the token tensor. token_data will not be empty.
        // NOLINTNEXTLINE(facebook-hte-LocalUncheckedArrayBounds)
        token_data[0] = cur_token;
      } else {
        // push it to the back
        token_data.push_back(cur_token);
        tokens_managed.resize({1, static_cast<int>(token_data.size())});
      }

      // print the token as string, decode it with the Tokenizer object
      token_callback(ET_UNWRAP(tokenizer_->decode(prev_token, cur_token)));

      if (should_stop_) {
        break;
      }

      // data-dependent terminating condition: we have n_eos_ number of EOS
      if (cur_token == eos_id_) {
        printf("\n");
        ET_LOG(Info, "\nReached to the end of generation");
        break;
      }
    }
    return pos - start_pos;
  }

  /**
   * Stop the generation loop.
   */
  inline void stop() {
    should_stop_ = true;
  }

 private:
  Tokenizer* tokenizer_;
  TextDecoderRunner* text_decoder_runner_;
  uint64_t eos_id_;
  bool use_kv_cache_;

  // state machine
  bool should_stop_ = false;

  // stats
  Stats* stats_;
};
} // namespace torch::executor
