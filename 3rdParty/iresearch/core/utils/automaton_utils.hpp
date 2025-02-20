﻿////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
////////////////////////////////////////////////////////////////////////////////

#ifndef IRESEARCH_AUTOMATON_UTILS_H
#define IRESEARCH_AUTOMATON_UTILS_H

#include "automaton.hpp"
#include "fst_states_map.hpp"
#include "hash_utils.hpp"
#include "utf8_utils.hpp"
#include "formats/formats.hpp"
#include "search/filter.hpp"

NS_ROOT

template<typename Char, typename Matcher>
inline automaton::Weight accept(
    const automaton& a,
    Matcher& matcher,
    const basic_string_ref<Char>& target) {
  auto state = a.Start();
  matcher.SetState(state);

  auto begin = target.begin();
  const auto end = target.end();

  for (; begin < end && matcher.Find(*begin); ++begin) {
    state = matcher.Value().nextstate;
    matcher.SetState(state);
  }

  return begin == end ? a.Final(state)
                      : automaton::Weight::Zero();
}

template<typename Char>
inline automaton::Weight accept(const automaton& a, const basic_string_ref<Char>& target) {
  typedef fst::RhoMatcher<fst::fsa::AutomatonMatcher> matcher_t;

  matcher_t matcher(a, fst::MatchType::MATCH_INPUT, fst::fsa::kRho);
  return accept(a, matcher, target);
}

class automaton_term_iterator final : public seek_term_iterator {
 public:
  automaton_term_iterator(const automaton& a, seek_term_iterator::ptr&& it)
    : a_(&a), matcher_(a_, fst::MatchType::MATCH_INPUT, fst::fsa::kRho), it_(std::move(it)) {
    assert(it_);
    value_ = &it_->value();
  }

  virtual const bytes_ref& value() const noexcept override {
    return *value_;
  }

  virtual doc_iterator::ptr postings(const flags& features) const override {
    return it_->postings(features);
  }

  virtual void read() override {
    it_->read();
  }

  virtual bool next() override {
    bool next = it_->next();

    while (next && !accept()) {
      next = it_->next();
    }

    return next;
  }

  virtual const attribute_view& attributes() const noexcept override {
    return it_->attributes();
  }

  virtual SeekResult seek_ge(const bytes_ref& target) override {
    it_->seek_ge(target);

    if (accept()) {
      return SeekResult::FOUND;
    }

    return next() ? SeekResult::NOT_FOUND : SeekResult::END;
  }

  virtual bool seek(const bytes_ref& target) override {
    return SeekResult::FOUND == seek_ge(target);
  }

  virtual bool seek(const bytes_ref& target, const seek_cookie& cookie) override {
    return it_->seek(target, cookie);
  }

  virtual seek_cookie::ptr cookie() const override {
    return it_->cookie();
  }

 private:
  typedef fst::RhoMatcher<fst::fsa::AutomatonMatcher> matcher_t;

  bool accept() { return irs::accept(*a_, matcher_, *value_); }

  const automaton* a_;
  matcher_t matcher_;
  seek_term_iterator::ptr it_;
  const bytes_ref* value_;
}; // automaton_term_iterator

//////////////////////////////////////////////////////////////////////////////
/// @class utf8_transitions_builder
/// @brief helper class for building minimal acyclic binary automaton from
///        a specified root, a default (rho) state and a set of arcs with
///        UTF-8 encoded labels
//////////////////////////////////////////////////////////////////////////////
class IRESEARCH_API utf8_transitions_builder {
 public:
  utf8_transitions_builder()
    : states_map_(16, state_emplace(weight_)) {
    // ensure we have enough space for utf8 sequence
    add_states(utf8_utils::MAX_CODE_POINT_SIZE);
  }

  template<typename Iterator>
  void insert(automaton& a,
              automaton::StateId from,
              automaton::StateId rho_state,
              Iterator begin, Iterator end) {
    // we inherit weight from 'from' node to all intermediate states
    // that were created by transitions builder
    weight_ = a.Final(from);
    last_ = bytes_ref::EMPTY;
    states_map_.reset();

    // 'from' state is already a part of automaton
    assert(!states_.empty());
    states_.front().id = from;

    std::fill(std::begin(rho_states_), std::end(rho_states_), rho_state);

    if (fst::kNoStateId != rho_state) {
      // create intermediate default states if necessary
      a.SetFinal(rho_states_[1] = a.AddState(), weight_);
      a.SetFinal(rho_states_[2] = a.AddState(), weight_);
      a.SetFinal(rho_states_[3] = a.AddState(), weight_);
    }

    for (; begin != end; ++begin) {
      // we expect sorted input
      assert(last_ <= std::get<0>(*begin));

      const auto& label = std::get<0>(*begin);
      insert(a, label.c_str(), label.size(), std::get<1>(*begin));
      last_ = label;
    }

    finish(a, from);
  }

 private:
  struct state;

  struct arc : private util::noncopyable {
    arc(automaton::Arc::Label label, state* target)
      : target(target),
        label(label) {
    }

    arc(arc&& rhs) noexcept
      : target(rhs.target),
        label(rhs.label) {
    }

    bool operator==(const automaton::Arc& rhs) const noexcept {
      return label == rhs.ilabel
        && id == rhs.nextstate;
    }

    bool operator!=(const automaton::Arc& rhs) const noexcept {
      return !(*this == rhs);
    }

    union {
      state* target;
      automaton::StateId id;
    };
    automaton::Arc::Label label;
  }; // arc

  struct state : private util::noncopyable {
    state() = default;

    state(state&& rhs) noexcept
      : rho_id(rhs.rho_id),
        id(rhs.id),
        arcs(std::move(rhs.arcs)) {
      rhs.rho_id = fst::kNoStateId;
      rhs.id = fst::kNoStateId;
    }

    void clear() noexcept {
      rho_id = fst::kNoStateId;
      id = fst::kNoStateId;
      arcs.clear();
    }

    automaton::StateId rho_id{fst::kNoStateId};
    automaton::StateId id{fst::kNoStateId};
    std::vector<arc> arcs;
  }; // state

  struct state_hash {
    size_t operator()(const state& s, const automaton& fst) const noexcept {
      if (fst::kNoStateId != s.id) {
        return operator()(s.id, fst);
      }

      size_t hash = 0;

      for (auto& arc: s.arcs) {
        hash = hash_combine(hash, arc.label);
        hash = hash_combine(hash, arc.id);
      }

      if (fst::kNoStateId != s.rho_id) {
        hash = hash_combine(hash, fst::fsa::kRho);
        hash = hash_combine(hash, s.rho_id);
      }

      return hash;
    }

    size_t operator()(automaton::StateId id, const automaton& fst) const noexcept {
      fst::ArcIteratorData<automaton::Arc> arcs;
      fst.InitArcIterator(id, &arcs);

      const auto* begin = arcs.arcs;
      const auto* end = arcs.arcs + arcs.narcs;

      size_t hash = 0;
      for (; begin != end; ++begin) {
        hash = hash_combine(hash, begin->ilabel);
        hash = hash_combine(hash, begin->nextstate);
      }

      return hash;
    }
  }; // state_hash

  struct state_equal {
    bool operator()(const state& lhs, automaton::StateId rhs, const automaton& fst) const noexcept {
      if (lhs.id != fst::kNoStateId) {
        // already a part of automaton
        return lhs.id == rhs;
      }

      fst::ArcIteratorData<automaton::Arc> rarcs;
      fst.InitArcIterator(rhs, &rarcs);

      const bool has_rho = (fst::kNoStateId != lhs.rho_id);

      if ((lhs.arcs.size() + size_t(has_rho)) != rarcs.narcs) {
        return false;
      }

      const auto* rarc = rarcs.arcs;
      for (const auto& larc : lhs.arcs) {
        if (larc != *rarc) {
          return false;
        }
        ++rarc;
      }

      if (has_rho && (rarc->ilabel != fst::fsa::kRho || rarc->nextstate != lhs.rho_id)) {
        return false;
      }

      return true;
    }
  }; // state_equal

  class state_emplace {
   public:
    explicit state_emplace(const automaton::Weight& weight) noexcept
      : weight_(&weight) {
    }

    automaton::StateId operator()(const state& s, automaton& fst) const {
      auto id = s.id;

      if (id == fst::kNoStateId) {
        id = fst.AddState();
        fst.SetFinal(id, *weight_);
      }

      for (const auto& a : s.arcs) {
        fst.EmplaceArc(id, a.label, a.id);
      }

      if (s.rho_id != fst::kNoStateId) {
        fst.EmplaceArc(id, fst::fsa::kRho, s.rho_id);
      }

      return id;
    }

   private:
    const automaton::Weight* weight_;
  }; // state_emplace

  using automaton_states_map = fst_states_map<
    automaton, state,
    state_emplace, state_hash,
    state_equal, fst::kNoStateId>;

  void add_states(size_t size) {
    // reserve size + 1 for root state
    if (states_.size() < ++size) {
      states_.resize(size);
    }
  }

  void minimize(automaton& a, size_t prefix) {
    assert(prefix > 0);

    for (size_t i = last_.size(); i >= prefix; --i) {
      state& s = states_[i];
      state& p = states_[i - 1];
      assert(!p.arcs.empty());

      p.arcs.back().id = states_map_.insert(s, a);

      s.clear();
    }
  }

  void insert(automaton& a,
              const byte_type* label_data,
              const size_t label_size,
              automaton::StateId target);

  void finish(automaton& a, automaton::StateId from);

  automaton::Weight weight_;
  automaton::StateId rho_states_[4];
  std::vector<state> states_;
  automaton_states_map states_map_;
  bytes_ref last_;
}; // utf8_automaton_builder

//////////////////////////////////////////////////////////////////////////////
/// @brief establish UTF-8 labeled connection between specified source and
///        target states
//////////////////////////////////////////////////////////////////////////////
IRESEARCH_API void utf8_emplace_arc(
  automaton& a,
  automaton::StateId from,
  const bytes_ref& label,
  automaton::StateId to);

//////////////////////////////////////////////////////////////////////////////
/// @brief establish UTF-8 labeled connection between specified source (from)
///        and target (to) states with the fallback to default (rho_state)
///        state
//////////////////////////////////////////////////////////////////////////////
IRESEARCH_API void utf8_emplace_arc(
  automaton& a,
  automaton::StateId from,
  automaton::StateId rho_state,
  const bytes_ref& label,
  automaton::StateId to);

//////////////////////////////////////////////////////////////////////////////
/// @brief establish default connnection between specified source (from) and
///        and target (to)
//////////////////////////////////////////////////////////////////////////////
IRESEARCH_API void utf8_emplace_rho_arc(
  automaton& a,
  automaton::StateId from,
  automaton::StateId to);

//////////////////////////////////////////////////////////////////////////////
/// @brief instantiate compiled filter based on a specified automaton, field
///        and other properties
/// @param field field name
/// @param acceptor input automaton
/// @param scored_terms_limit score as many terms
/// @param index index reader
/// @param order compiled order
/// @param bool query boost
/// @returns compiled filter
//////////////////////////////////////////////////////////////////////////////
IRESEARCH_API filter::prepared::ptr prepare_automaton_filter(
  const string_ref& field,
  const automaton& acceptor,
  size_t scored_terms_limit,
  const index_reader& index,
  const order::prepared& order,
  boost_t boost);

NS_END

#endif
