/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "model.h"

#include <assert.h>

#include <algorithm>

#include <cmath>

#include "utils.h"


namespace fasttext {

Model::Model(std::shared_ptr<Matrix> wi, std::shared_ptr<Matrix> wo,
             std::shared_ptr<Matrix> attn, std::shared_ptr<Vector> bias,
             std::shared_ptr<Args> args, int32_t seed)
    : hidden_(args->dim), output_(wo->m_), grad_(args->dim), rng(seed) {
  wi_ = wi;
  wo_ = wo;
  attn_ = attn;
  bias_ = bias;
  args_ = args;
  isz_ = wi->m_;
  osz_ = wo->m_;
  hsz_ = args->dim;
  negpos = 0;
  loss_ = 0.0;
  nexamples_ = 1;
  initSigmoid();
  initLog();
}

Model::~Model() {
  delete[] t_sigmoid;
  delete[] t_log;
}

real Model::binaryLogistic(int32_t target, bool label, real lr) {
  real score = sigmoid(wo_->dotRow(hidden_, target));
  real alpha = lr * (real(label) - score);
  grad_.addRow(*wo_, target, alpha);
  wo_->addRow(hidden_, target, alpha);
  if (label) {
    return -log(score);
  } else {
    return -log(1.0 - score);
  }
}

real Model::negativeSampling(int32_t target, real lr) {
  real loss = 0.0;
  grad_.zero();
  for (int32_t n = 0; n <= args_->neg; n++) {
    if (n == 0) {
      loss += binaryLogistic(target, true, lr);
    } else {
      loss += binaryLogistic(getNegative(target), false, lr);
    }
  }
  // std::cout << "grad_" << std::endl;
  // std::cout << grad_ << std::endl;
  return loss;
}

real Model::hierarchicalSoftmax(int32_t target, real lr) {
  real loss = 0.0;
  grad_.zero();
  const std::vector<bool>& binaryCode = codes[target];
  const std::vector<int32_t>& pathToRoot = paths[target];
  for (int32_t i = 0; i < pathToRoot.size(); i++) {
    loss += binaryLogistic(pathToRoot[i], binaryCode[i], lr);
  }
  return loss;
}

void Model::computeOutputSoftmax(Vector& hidden, Vector& output) const {
  output.mul(*wo_, hidden);
  real max = output[0], z = 0.0;
  for (int32_t i = 0; i < osz_; i++) {
    max = std::max(output[i], max);
  }
  for (int32_t i = 0; i < osz_; i++) {
    output[i] = exp(output[i] - max);
    z += output[i];
  }
  for (int32_t i = 0; i < osz_; i++) {
    output[i] /= z;
  }
}

void Model::computeOutputSoftmax() { computeOutputSoftmax(hidden_, output_); }

real Model::softmax(int32_t target, real lr) {
  grad_.zero();
  computeOutputSoftmax();
  for (int32_t i = 0; i < osz_; i++) {
    real label = (i == target) ? 1.0 : 0.0;
    real alpha = lr * (label - output_[i]);
    grad_.addRow(*wo_, i, alpha);
    wo_->addRow(hidden_, i, alpha);
  }
  return -log(output_[target]);
}

void Model::computeHidden(const std::vector<int32_t>& input,
                          Vector& hidden) const {
  assert(hidden.size() == hsz_);
  hidden.zero();
  for (auto it = input.cbegin(); it != input.cend(); ++it) {
    hidden.addRow(*wi_, *it);
  }
  hidden.mul(1.0 / input.size());
}

/*
  computeAttnHidden: compute hidden vector in attention model (context view).
  Args:
    input: a pair vector; the first is the context feature, and the second is
  the relative position.
    hidden: the hidden vector.
*/
void Model::computeAttnHidden(
    const std::vector<std::pair<int32_t, int32_t>>& input, Vector& hidden,
    std::vector<real>& softmaxattn) const {
  assert(hidden.size() == hsz_);
  /*
  std::cout << "input line" << std::endl;
  for (auto item : input) {
    std::cout << item.first << " " << item.second << std::endl;
  }
  */
  hidden.zero();
  softmaxattn.clear();
  real sum = 0.0;
  real attention_max = 0.0;
  real attention_i = 0.0;
  std::vector<real> attention;

  for (int32_t i = 0; i < input.size(); i++) {
    attention_i =
        (*attn_)(input[i].first, input[i].second) + (*bias_)[input[i].second];
    if (attention_i > attention_max) {
      attention_max = attention_i;
    }
    attention.push_back(attention_i);
  }
  for (int32_t i = 0; i < input.size(); i++) {
    if (attention.at(i) - attention_max < -50)
      softmaxattn.push_back(0);
    else
      softmaxattn.push_back(std::exp(attention.at(i) - attention_max));
    sum += softmaxattn.at(i);
  }
  for (int32_t i = 0; i < input.size(); i++) softmaxattn.at(i) /= sum;
  for (int32_t i = 0; i < input.size(); i++)
    hidden.addRow(*wi_, input[i].first, softmaxattn.at(i));
  // std::cout << "hidden" << std::endl;
  // std::cout << hidden << std::endl;
  /*
  std::cout << "attention" << std::endl;
  for (int32_t i = 0; i < softmaxattn.size(); i++) {
    std::cout << softmaxattn[i] << " ";
  }
  std::cout << std::endl;
  */
}

/*
  computeAttnHidden2: compute hidden vector in attention model (feature view).
  Args:
    input: a pair vector; the first is the context feature, and the second is
  the relative position.
    hidden: the hidden vector.
*/
void Model::computeAttnHidden2(
    const std::vector<std::pair<int32_t, int32_t>>& input, int32_t target,
    Vector& hidden, std::vector<real>& softmaxattn) const {
  assert(hidden.size() == hsz_);
  hidden.zero();
  softmaxattn.clear();
  real sum = 0.0;
  real attention_max = 0.0;
  real attention_i  = 0.0;
  std::vector<real> attention;
  for (int32_t i = 0; i < input.size(); i++) {
    attention_i = (*attn_)(target, input[i].second) + (*bias_)[input[i].second];
    if (attention_i > attention_max)
      attention_max = attention_i;
    attention.push_back(attention_i);
  }
  for (int32_t i = 0; i < input.size(); i++) {
    if (attention.at(i) - attention_max < -50)
      softmaxattn.push_back(0.0);
    else
      softmaxattn.push_back(std::exp(attention.at(i) - attention_max));
    sum += softmaxattn.at(i);
  }
  for (int32_t i = 0; i < input.size(); i++) softmaxattn.at(i) /= sum;
  for (int32_t i = 0; i < input.size(); i++)
    hidden.addRow(*wi_, input[i].first, softmaxattn[i]);
}

bool Model::comparePairs(const std::pair<real, int32_t>& l,
                         const std::pair<real, int32_t>& r) {
  return l.first > r.first;
}

void Model::predict(const std::vector<int32_t>& input, int32_t k,
                    std::vector<std::pair<real, int32_t>>& heap, Vector& hidden,
                    Vector& output) const {
  assert(k > 0);
  heap.reserve(k + 1);
  computeHidden(input, hidden);
  if (args_->loss == loss_name::hs) {
    dfs(k, 2 * osz_ - 2, 0.0, heap, hidden);
  } else {
    findKBest(k, heap, hidden, output);
  }
  std::sort_heap(heap.begin(), heap.end(), comparePairs);
}

void Model::predict(const std::vector<int32_t>& input, int32_t k,
                    std::vector<std::pair<real, int32_t>>& heap) {
  predict(input, k, heap, hidden_, output_);
}

void Model::findKBest(int32_t k, std::vector<std::pair<real, int32_t>>& heap,
                      Vector& hidden, Vector& output) const {
  computeOutputSoftmax(hidden, output);
  for (int32_t i = 0; i < osz_; i++) {
    if (heap.size() == k && log(output[i]) < heap.front().first) {
      continue;
    }
    heap.push_back(std::make_pair(log(output[i]), i));
    std::push_heap(heap.begin(), heap.end(), comparePairs);
    if (heap.size() > k) {
      std::pop_heap(heap.begin(), heap.end(), comparePairs);
      heap.pop_back();
    }
  }
}

void Model::dfs(int32_t k, int32_t node, real score,
                std::vector<std::pair<real, int32_t>>& heap,
                Vector& hidden) const {
  if (heap.size() == k && score < heap.front().first) {
    return;
  }

  if (tree[node].left == -1 && tree[node].right == -1) {
    heap.push_back(std::make_pair(score, node));
    std::push_heap(heap.begin(), heap.end(), comparePairs);
    if (heap.size() > k) {
      std::pop_heap(heap.begin(), heap.end(), comparePairs);
      heap.pop_back();
    }
    return;
  }

  real f = sigmoid(wo_->dotRow(hidden, node - osz_));
  dfs(k, tree[node].left, score + log(1.0 - f), heap, hidden);
  dfs(k, tree[node].right, score + log(f), heap, hidden);
}

/*
  computeAttnGradient: compute gradients for input vectors and attention
  parameters (context view).
  Args:
    input: a pair vector; the first is the context feature, and the second is
  the relative position.
    gradient: the gradient vector.
*/
void Model::computeAttnGradient(
    const std::vector<std::pair<int32_t, int32_t>>& input, Vector& gradient,
    std::vector<real>& softmaxattn) const {
  assert(gradient.size() == hsz_);
  /*
  std::cout << "attention" << std::endl;
  for (int32_t i = 0; i < softmaxattn.size(); i++) {
    std::cout << softmaxattn[i] << " ";
  }
  std::cout << std::endl;
  */
  int32_t input_size = input.size();
  // std::cout << gradient << std::endl;
  // for (int32_t i = 0; i < input_size; i++) {
  //     std::cout << softmaxattn.at(i) << std::endl;
  // }
  for (int32_t i = 0; i < input_size; i++) {
    // update attention parameters
    // real gattn = softmaxattn.at(i) * (1 - softmaxattn.at(i)) *
    //             (wi_->dotRow(gradient, input[i].first) - gradient.dot(hidden_);

    real gattn = softmaxattn.at(i) * (wi_->dotRow(gradient, input[i].first) - gradient.dot(hidden_));
    // update input vectors
    // std::cout << "gattn: " << gattn << std::endl;
    wi_->addRow(gradient, input[i].first, softmaxattn.at(i) * input_size);
    // use hidden_ vector to avoid overflow?
    // real gattn = softmaxattn.at(i) * (1 - softmaxattn.at(i)) *
    //     (wi_->dotRow(gradient, input[i].first) - gradient.dot(hidden_));
    (*attn_)(input[i].first, input[i].second) += gattn;
    (*bias_)[input[i].second] += gattn;
  }
}

/*
  computeAttnGradient2: compute gradients for input vectors and attention
  parameters (feature view).
  Args:
    input: a pair vector; the first is the context feature, and the second is
  the relative position.
    gradient: the gradient vector.
*/
void Model::computeAttnGradient2(
    const std::vector<std::pair<int32_t, int32_t>>& input, int32_t target,
    Vector& gradient, std::vector<real>& softmaxattn) const {
  assert(gradient.size() == hsz_);
  int32_t input_size = input.size();
  for (int32_t i = 0; i < input.size(); i++) {
    // update attention parameters
    real gattn = softmaxattn[i] * (wi_->dotRow(gradient, input[i].first) - gradient.dot(hidden_));
    // update input vectors
    wi_->addRow(gradient, input[i].first, softmaxattn[i] * input_size);
    (*attn_)(target, input[i].second) += gattn;
    (*bias_)[input[i].second] += gattn;
  }
}

/*
  updateAttn: update the attention model (context view).
  Args:
    input: a pair vector; the first is the context feature, and the second is
  the relative position;
    target: the target feature;
    lr: learning rate.
*/
void Model::updateAttn(std::vector<std::pair<int32_t, int32_t>>& input,
                       int32_t target, real lr) {
  assert(target >= 0);
  assert(target < osz_);
  if (input.size() == 0) return;
  softmaxattn_.clear();
  std::vector<real>().swap(softmaxattn_);
  // erase contexts that are the same to the target
  // std::vector<std::pair<int32_t, int32_t>>::iterator iter;
  for (auto iter = input.begin(); iter != input.end();) {
    if (iter->first == target)
      iter = input.erase(iter);
    else
      iter++;
  }
  if (input.size() == 0) return;
  computeAttnHidden(input, hidden_, softmaxattn_);
  // std::cout << "hidden l1: " << hidden_.l1() << std::endl;
  if (args_->loss == loss_name::ns) {
    loss_ += negativeSampling(target, lr);
  } else if (args_->loss == loss_name::hs) {
    loss_ += hierarchicalSoftmax(target, lr);
  } else {
    loss_ += softmax(target, lr);
  }
  nexamples_ += 1;

  computeAttnGradient(input, grad_, softmaxattn_);
}

/*
  updateAttn2: update the attention model (feature view).
  Args:
    input: a pair vector; the first is the context feature, and the second is
  the relative position;
    target: the target feature;
    lr: learning rate.
*/
void Model::updateAttn2(std::vector<std::pair<int32_t, int32_t>>& input,
                        int32_t target, real lr) {
  assert(target >= 0);
  assert(target < osz_);
  if (input.size() == 0) return;
  softmaxattn_.clear();
  std::vector<real>().swap(softmaxattn_);
  // erase contexts that are the same to the target
  // std::vector<std::pair<int32_t, int32_t>>::iterator iter;
  for (auto iter = input.begin(); iter != input.end();) {
    if (iter->first == target)
      iter = input.erase(iter);
    else
      iter++;
  }
  if (input.size() == 0) return;
  computeAttnHidden2(input, target, hidden_, softmaxattn_);
  // std::cout << "hidden l1: " << hidden_.l1() << std::endl;
  if (args_->loss == loss_name::ns) {
    loss_ += negativeSampling(target, lr);
  } else if (args_->loss == loss_name::hs) {
    loss_ += hierarchicalSoftmax(target, lr);
  } else {
    loss_ += softmax(target, lr);
  }
  nexamples_ += 1;

  computeAttnGradient2(input, target, grad_, softmaxattn_);
}

void Model::update(const std::vector<int32_t>& input, int32_t target, real lr) {
  assert(target >= 0);
  assert(target < osz_);
  if (input.size() == 0) return;
  computeHidden(input, hidden_);
  if (args_->loss == loss_name::ns) {
    loss_ += negativeSampling(target, lr);
  } else if (args_->loss == loss_name::hs) {
    loss_ += hierarchicalSoftmax(target, lr);
  } else {
    loss_ += softmax(target, lr);
  }
  nexamples_ += 1;

  if (args_->model == model_name::sup) {
    grad_.mul(1.0 / input.size());
  }
  for (auto it = input.cbegin(); it != input.cend(); ++it) {
    wi_->addRow(grad_, *it, 1.0);
  }
}

void Model::setTargetCounts(const std::vector<int64_t>& counts) {
  assert(counts.size() == osz_);
  if (args_->loss == loss_name::ns) {
    initTableNegatives(counts);
  }
  if (args_->loss == loss_name::hs) {
    buildTree(counts);
  }
}

void Model::initTableNegatives(const std::vector<int64_t>& counts) {
  real z = 0.0;
  for (size_t i = 0; i < counts.size(); i++) {
    z += pow(counts[i], 0.5);
  }
  for (size_t i = 0; i < counts.size(); i++) {
    real c = pow(counts[i], 0.5);
    for (size_t j = 0; j < c * NEGATIVE_TABLE_SIZE / z; j++) {
      negatives.push_back(i);
    }
  }
  std::shuffle(negatives.begin(), negatives.end(), rng);
}

int32_t Model::getNegative(int32_t target) {
  int32_t negative;
  do {
    negative = negatives[negpos];
    negpos = (negpos + 1) % negatives.size();
  } while (target == negative);
  return negative;
}

void Model::buildTree(const std::vector<int64_t>& counts) {
  tree.resize(2 * osz_ - 1);
  for (int32_t i = 0; i < 2 * osz_ - 1; i++) {
    tree[i].parent = -1;
    tree[i].left = -1;
    tree[i].right = -1;
    tree[i].count = 1e15;
    tree[i].binary = false;
  }
  for (int32_t i = 0; i < osz_; i++) {
    tree[i].count = counts[i];
  }
  int32_t leaf = osz_ - 1;
  int32_t node = osz_;
  for (int32_t i = osz_; i < 2 * osz_ - 1; i++) {
    int32_t mini[2];
    for (int32_t j = 0; j < 2; j++) {
      if (leaf >= 0 && tree[leaf].count < tree[node].count) {
        mini[j] = leaf--;
      } else {
        mini[j] = node++;
      }
    }
    tree[i].left = mini[0];
    tree[i].right = mini[1];
    tree[i].count = tree[mini[0]].count + tree[mini[1]].count;
    tree[mini[0]].parent = i;
    tree[mini[1]].parent = i;
    tree[mini[1]].binary = true;
  }
  for (int32_t i = 0; i < osz_; i++) {
    std::vector<int32_t> path;
    std::vector<bool> code;
    int32_t j = i;
    while (tree[j].parent != -1) {
      path.push_back(tree[j].parent - osz_);
      code.push_back(tree[j].binary);
      j = tree[j].parent;
    }
    paths.push_back(path);
    codes.push_back(code);
  }
}

real Model::getLoss() const { return loss_ / nexamples_; }

void Model::initSigmoid() {
  t_sigmoid = new real[SIGMOID_TABLE_SIZE + 1];
  for (int i = 0; i < SIGMOID_TABLE_SIZE + 1; i++) {
    real x = real(i * 2 * MAX_SIGMOID) / SIGMOID_TABLE_SIZE - MAX_SIGMOID;
    t_sigmoid[i] = 1.0 / (1.0 + std::exp(-x));
  }
}

void Model::initLog() {
  t_log = new real[LOG_TABLE_SIZE + 1];
  for (int i = 0; i < LOG_TABLE_SIZE + 1; i++) {
    real x = (real(i) + 1e-5) / LOG_TABLE_SIZE;
    t_log[i] = std::log(x);
  }
}

real Model::log(real x) const {
  if (x > 1.0) {
    return 0.0;
  }
  int i = int(x * LOG_TABLE_SIZE);
  return t_log[i];
}

real Model::sigmoid(real x) const {
  if (x < -MAX_SIGMOID) {
    return 0.0;
  } else if (x > MAX_SIGMOID) {
    return 1.0;
  } else {
    int i = int((x + MAX_SIGMOID) * SIGMOID_TABLE_SIZE / MAX_SIGMOID / 2);
    return t_sigmoid[i];
  }
}
}
