////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
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
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "tests_shared.hpp"
#include "search/all_filter.hpp"
#include "search/all_iterator.hpp"
#include "search/boolean_filter.hpp"
#include "search/range_filter.hpp"
#include "search/disjunction.hpp"
#include "search/min_match_disjunction.hpp"
#include "search/exclusion.hpp"
#include "search/bm25.hpp"
#include "search/tfidf.hpp"
#include "filter_test_case_base.hpp"
#include "index/iterators.hpp"
#include "formats/formats.hpp"
#include "search/term_filter.hpp"
#include "search/term_query.hpp"

#include <functional>

// ----------------------------------------------------------------------------
// --SECTION--                                                   Iterator tests
// ----------------------------------------------------------------------------

NS_BEGIN(tests)
NS_BEGIN(detail)

struct basic_sort : irs::sort {
  DECLARE_SORT_TYPE();

  static irs::sort::ptr make(size_t i) {
    return irs::sort::ptr(new basic_sort(i));
  }

  explicit basic_sort(size_t idx)
    : irs::sort(basic_sort::type()), idx(idx) {
  }

  struct basic_scorer final : irs::score_ctx {
    explicit basic_scorer(size_t idx) : idx(idx) {}

    size_t idx;
  };

  struct prepared_sort final : irs::sort::prepared {
    explicit prepared_sort(size_t idx) : idx(idx) {
      merge_func_ = [](const irs::order_bucket* ctx, irs::byte_type* dst,
                       const irs::byte_type** src_start, const size_t size) {
        const auto offset = ctx->score_offset;
        score_cast<size_t>(dst + offset) = 0;
        for (size_t i = 0; i < size; ++i) {
          score_cast<size_t>(dst + offset) += score_cast<size_t>(src_start[i]  + offset);
        }
      };
    }

    virtual void collect(
      irs::byte_type* filter_attrs,
      const irs::index_reader& index,
      const irs::sort::field_collector* field,
      const irs::sort::term_collector* term
    ) const override {
      // do not need to collect stats
    }

    const irs::flags& features() const override {
      return irs::flags::empty_instance();
    }

    virtual irs::sort::field_collector::ptr prepare_field_collector() const override {
      return nullptr; // do not need to collect stats
    }

    std::pair<irs::score_ctx_ptr, irs::score_f> prepare_scorer(
        const irs::sub_reader&,
        const irs::term_reader&,
        const irs::byte_type*,
        const irs::attribute_view&,
        irs::boost_t
    ) const override {
      return {
        irs::score_ctx_ptr(new basic_scorer(idx)),
        [](const irs::score_ctx* ctx, irs::byte_type* score) {
          auto& state = *reinterpret_cast<const basic_scorer*>(ctx);
          sort::score_cast<size_t>(score) = state.idx;
        }
      };
    }

    void prepare_stats(irs::byte_type*) const override { }

    void prepare_score(irs::byte_type* score) const override {
      score_cast<size_t>(score) = 0;
    }

    virtual irs::sort::term_collector::ptr prepare_term_collector() const override {
      return nullptr; // do not need to collect stats
    }

    bool less(const irs::byte_type* lhs, const irs::byte_type* rhs) const override {
      return score_cast<size_t>(lhs) < score_cast<size_t>(rhs);
    }

    std::pair<size_t, size_t> score_size() const override {
      return std::make_pair(sizeof(size_t), alignof(size_t));
    }

    std::pair<size_t, size_t> stats_size() const override {
      return std::make_pair(size_t(0), size_t(0));
    }

    size_t idx;
  };

  irs::sort::prepared::ptr prepare() const override {
    return irs::sort::prepared::ptr(new prepared_sort(idx));
  }

  size_t idx;
};

DEFINE_SORT_TYPE(::tests::detail::basic_sort)

class basic_doc_iterator: public irs::doc_iterator, irs::score_ctx {
 public:
  typedef std::vector<irs::doc_id_t> docids_t;

#if defined(_MSC_VER)
  #pragma warning( disable : 4706 )
#elif defined (__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wparentheses"
#endif

  basic_doc_iterator(
      const docids_t::const_iterator& first,
      const docids_t::const_iterator& last,
      const irs::byte_type* stats = nullptr,
      const irs::order::prepared& ord = irs::order::prepared::unordered(),
      irs::boost_t boost = irs::no_boost())
    : first_(first),
      last_(last),
      stats_(stats),
      doc_(irs::doc_limits::invalid()) {
    est_.value(std::distance(first_, last_));
    attrs_.emplace(est_);
    attrs_.emplace(doc_);

    if (!ord.empty()) {
      assert(stats_);

      scorers_ = ord.prepare_scorers(
        irs::sub_reader::empty(),
        empty_term_reader::instance(),
        stats_,
        attrs_,
        boost
      );

      score_.prepare(ord, this, [](const irs::score_ctx* ctx, irs::byte_type* score) {
        auto& self = *static_cast<const basic_doc_iterator*>(ctx);
        self.scorers_.score(score);
      });

      attrs_.emplace(score_);
    }
  }

#if defined(_MSC_VER)
  #pragma warning( default : 4706 )
#elif defined (__GNUC__)
  #pragma GCC diagnostic pop
#endif

  virtual irs::doc_id_t value() const override { return doc_.value; }

  virtual bool next() override {
    if ( first_ == last_ ) {
      doc_ = irs::type_limits<irs::type_t::doc_id_t>::eof();
      return false;
    }

    doc_ = *first_;
    ++first_;
    return true;
  }

  virtual const irs::attribute_view& attributes() const noexcept override {
    return attrs_;
  }

  virtual irs::doc_id_t seek(irs::doc_id_t doc) override {
    if (irs::type_limits<irs::type_t::doc_id_t>::eof(doc_.value) || doc <= doc_.value) {
      return doc_.value;
    }

    do { 
      next(); 
    } while (doc_.value < doc);

    return doc_.value;
  }

 private:
  irs::cost est_;
  irs::attribute_view attrs_;
  irs::order::prepared::scorers scorers_;
  docids_t::const_iterator first_;
  docids_t::const_iterator last_;
  const irs::byte_type* stats_;
  irs::score score_;
  irs::document doc_;
}; // basic_doc_iterator

std::vector<irs::doc_id_t> union_all(
    const std::vector<std::vector<irs::doc_id_t>>& docs
) {
  std::vector<irs::doc_id_t> result;
  for(auto& part : docs) {
    std::copy(part.begin(), part.end(), std::back_inserter(result));
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

template<typename DocIterator>
std::vector<DocIterator> execute_all(
    const std::vector<std::vector<irs::doc_id_t>>& docs
) {
  std::vector<DocIterator> itrs;
  itrs.reserve(docs.size());
  for (const auto& doc : docs) {
    itrs.emplace_back(irs::doc_iterator::make<detail::basic_doc_iterator>(
      doc.begin(), doc.end()
    ));
  }

  return itrs;
}

template<typename DocIterator>
std::pair<std::vector<DocIterator>, std::vector<irs::order::prepared>> execute_all(
    const std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>>& docs
) {
  const irs::byte_type* stats = irs::bytes_ref::EMPTY.c_str();
  std::vector<irs::order::prepared> order;
  order.reserve(docs.size());
  std::vector<DocIterator> itrs;
  itrs.reserve(docs.size());
  for (const auto& entry : docs) {
    auto& doc = entry.first;
    auto& ord = entry.second;

    if (ord.empty()) {
      itrs.emplace_back(irs::doc_iterator::make<detail::basic_doc_iterator>(
        doc.begin(), doc.end()
      ));
    } else {
      order.emplace_back(ord.prepare());

      itrs.emplace_back(irs::doc_iterator::make<detail::basic_doc_iterator>(
        doc.begin(), doc.end(), stats, order.back(), irs::no_boost()
      ));
    }
  }

  return { std::move(itrs), std::move(order) };
}

struct seek_doc {
  irs::doc_id_t target;
  irs::doc_id_t expected;
};

NS_END // detail

// ----------------------------------------------------------------------------
// --SECTION--                                              Boolean query boost
// ----------------------------------------------------------------------------

NS_BEGIN(detail)

struct boosted: public irs::filter {
  struct prepared: irs::filter::prepared {
    explicit prepared(
        const basic_doc_iterator::docids_t& docs,
        irs::boost_t boost)
      : irs::filter::prepared(boost), docs(docs) {
    }

    virtual irs::doc_iterator::ptr execute(
      const irs::sub_reader& rdr,
      const irs::order::prepared& ord,
      const irs::attribute_view& /*ctx*/
    ) const override {
      return irs::doc_iterator::make<basic_doc_iterator>(
        docs.begin(), docs.end(), stats.c_str(), ord, boost()
      );
    }

    basic_doc_iterator::docids_t docs;
    irs::bstring stats;
  }; // prepared

  DECLARE_FACTORY();

  virtual irs::filter::prepared::ptr prepare(
      const irs::index_reader&,
      const irs::order::prepared&,
      irs::boost_t boost,
      const irs::attribute_view& /*ctx*/) const override {
    return filter::prepared::make<boosted::prepared>(docs, this->boost()*boost);
  }

  DECLARE_FILTER_TYPE();
  boosted(): filter(boosted::type()) { }

  basic_doc_iterator::docids_t docs;
}; // boosted

DEFINE_FILTER_TYPE(boosted)
DEFINE_FACTORY_DEFAULT(boosted)

NS_END // detail

TEST(boolean_query_boost, hierarchy) {
  // hierarchy of boosted subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::And root;
    root.boost(value);
    {
      auto& sub = root.add<irs::Or>();
      sub.boost(value);
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2, 3 };
        node.boost(value);
      }
    }

    {
      auto& sub = root.add<irs::Or>();
      sub.boost(value);
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2, 3 };
        node.boost(value);
      }
    }

    {
      auto& sub = root.add<detail::boosted>();
      sub.docs = { 1, 2 };
      sub.boost(value);
    }

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      pord
    );

    auto docs = prep->execute(
      irs::sub_reader::empty(), pord
    );

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);

    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    /* the first hit should be scored as 2*value^3 +2*value^3+value^2 since it
     * exists in all results */
    {
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(docs->value(), doc->value);
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(4*value*value*value+value*value, doc_boost);
    }

    /* the second hit should be scored as 2*value^3+value^2 since it
     * exists in all results */
    {
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(docs->value(), doc->value);
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(4*value*value*value+value*value, doc_boost);
    }

    ASSERT_FALSE(docs->next());
  }

  // hierarchy of boosted subqueries (multiple Or's)
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::And root;
    root.boost(value);
    {
      auto& sub = root.add<irs::Or>();
      sub.boost(value);
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 3 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2 };
      }
    }

    {
      auto& sub = root.add<irs::Or>();
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2, 3 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1 };
        node.boost(value);
      }
    }

    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2, 3 };
    }

    auto prep = root.prepare(
        irs::sub_reader::empty(),
        pord
    );

    auto docs = prep->execute(
      irs::sub_reader::empty(), pord
    );

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);

    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    /* the first hit should be scored as 2*value^3+value^2+3*value^2+value
     * since it exists in all results */
    {
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(docs->value(), doc->value);
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(2*value*value*value+4*value*value+value, doc_boost);
    }

    /* the second hit should be scored as value^3+value^2+2*value^2 since it
     * exists in all results */
    {
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(docs->value(), doc->value);
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(value*value*value+3*value*value+value, doc_boost);
    }

    /* the third hit should be scored as value^3+value^2 since it
     * exists in all results */
    {
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(docs->value(), doc->value);
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(value*value*value+value*value+value, doc_boost);
    }

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

  // hierarchy of boosted subqueries (multiple And's)
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::Or root;
    root.boost(value);
    {
      auto& sub = root.add<irs::And>();
      sub.boost(value);
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2 };
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 3 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2 };
      }
    }

    {
      auto& sub = root.add<irs::And>();
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1, 2, 3 };
        node.boost(value);
      }
      {
        auto& node = sub.add<detail::boosted>();
        node.docs = { 1 };
        node.boost(value);
      }
    }

    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2, 3 };
    }

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      pord
    );

    auto docs = prep->execute(irs::sub_reader::empty(), pord);

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);

    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // the first hit should be scored as value^3+2*value^2+3*value^2+value
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(value*value*value+5*value*value+value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    // the second hit should be scored as value
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    // the third hit should be scored as value
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }
}

TEST(boolean_query_boost, and) {
  // empty boolean unboosted query
  {
    irs::And root;

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    ASSERT_EQ(irs::no_boost(), prep->boost());
  }

  // boosted empty boolean query
  {
    const irs::boost_t value = 5;

    irs::And root;
    root.boost(value);

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    ASSERT_EQ(irs::no_boost(), prep->boost());
  }

  // single boosted subquery
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::And root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      pord
    );

    auto docs = prep->execute(irs::sub_reader::empty(), pord);

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);
    ASSERT_TRUE(docs->next());
    scr->evaluate();
    auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0) ;
    ASSERT_EQ(value, doc_boost);
    ASSERT_FALSE(docs->next());
  }

  // boosted root & single boosted subquery
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::And root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }
    root.boost(value);

    auto prep = root.prepare(
      irs::sub_reader::empty(),
        pord
    );

    auto docs = prep->execute(irs::sub_reader::empty(), pord);

    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);
    ASSERT_TRUE(docs->next());
    scr->evaluate();
    auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0) ;
    ASSERT_EQ(value*value, doc_boost);
    ASSERT_EQ(docs->value(), doc->value);
    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

  // boosted root & several boosted subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::And root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }
    root.boost(value);

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      pord
    );

    auto docs = prep->execute(irs::sub_reader::empty(), pord);

    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    /* the first hit should be scored as value*value + value*value since it
     * exists in both results */
    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);
    ASSERT_TRUE(docs->next());
    scr->evaluate();
    auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0) ;
    ASSERT_EQ(2*value*value, doc_boost);
    ASSERT_EQ(docs->value(), doc->value);

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

  // boosted root & several boosted subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::And root;
    root.boost(value);
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }

    auto prep = root.prepare(irs::sub_reader::empty(), pord);
    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);
    ASSERT_EQ(docs->value(), doc->value);
    ASSERT_TRUE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
    scr->evaluate();
    auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0) ;
    ASSERT_EQ(3*value*value+value, doc_boost);

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

   // unboosted root & several boosted subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::And root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(0.f);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }

    auto prep = root.prepare(irs::sub_reader::empty(), pord);
    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);
    ASSERT_EQ(docs->value(), doc->value);
    ASSERT_TRUE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
    scr->evaluate();
    auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0) ;
    ASSERT_EQ(3*value, doc_boost);

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }


  // unboosted root & several unboosted subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::And root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(0.f);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(0.f);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(0.f);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(0.f);
    }

    auto prep = root.prepare(irs::sub_reader::empty(), pord);
    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);
    ASSERT_EQ(docs->value(), doc->value);
    ASSERT_TRUE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
    scr->evaluate();
    auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0) ;
    ASSERT_EQ(irs::boost_t(0), doc_boost);

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }
}

TEST(boolean_query_boost, or) {
  // single unboosted query
  {
    const irs::boost_t value = 5;

    irs::Or root;

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    ASSERT_EQ(irs::no_boost(), prep->boost());
  }

  // empty single boosted query
  {
    const irs::boost_t value = 5;

    irs::Or root;
    root.boost(value);

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    ASSERT_EQ(irs::no_boost(), prep->boost());
  }

  // boosted empty single query
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::Or root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
    }
    root.boost(value);

    auto prep = root.prepare(irs::sub_reader::empty(), pord);
    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);
    ASSERT_EQ(docs->value(), doc->value);
    ASSERT_TRUE(docs->next());
    scr->evaluate();
    auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
    ASSERT_EQ(value, doc_boost);
    ASSERT_EQ(docs->value(), doc->value);
    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

  // boosted single query & subquery
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::Or root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }
    root.boost(value);

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      pord
    );

    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);
    ASSERT_EQ(docs->value(), doc->value);
    ASSERT_TRUE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
    scr->evaluate();
    auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
    ASSERT_EQ(value*value, doc_boost);
    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

  // boosted single query & several subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::Or root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }
    root.boost(value);

    auto prep = root.prepare(irs::sub_reader::empty(), pord);
    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);

    // the first hit should be scored as value*value + value*value since it
    // exists in both results
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(2 * value * value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    // the second hit should be scored as value*value since it
    // exists in second result only
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(value * value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

  // boosted root & several boosted subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::Or root;
    root.boost(value);

    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }

    auto prep = root.prepare(irs::sub_reader::empty(), pord);
    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);

    // first hit
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(3*value*value + value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    // second hit
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(2*value*value + value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

  // unboosted root & several boosted subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::Or root;

    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(0.f);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(value);
    }

    auto prep = root.prepare(irs::sub_reader::empty(), pord);
    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);

    // first hit
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(3*value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    // second hit
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(2*value, doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }

  // unboosted root & several unboosted subqueries
  {
    const irs::boost_t value = 5;

    irs::order ord;
    ord.add<tests::sort::boost>(false);
    auto pord = ord.prepare();

    irs::Or root;
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1 };
      node.boost(0.f);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(0.f);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(0.f);
    }
    {
      auto& node = root.add<detail::boosted>();
      node.docs = { 1, 2 };
      node.boost(0.f);
    }

    auto prep = root.prepare(irs::sub_reader::empty(), pord);
    auto docs = prep->execute(irs::sub_reader::empty(), pord);
    auto& doc = docs->attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    auto& scr = docs->attributes().get<irs::score>();
    ASSERT_FALSE(!scr);

    // first hit
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(irs::boost_t(0), doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    // second hit
    {
      ASSERT_TRUE(docs->next());
      scr->evaluate();
      auto doc_boost = pord.get<tests::sort::boost::score_t>(scr->c_str(), 0);
      ASSERT_EQ(irs::boost_t(0), doc_boost);
      ASSERT_EQ(docs->value(), doc->value);
    }

    ASSERT_FALSE(docs->next());
    ASSERT_EQ(docs->value(), doc->value);
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                                         Boolean query estimation
// ----------------------------------------------------------------------------

NS_BEGIN(detail)

struct unestimated: public irs::filter {
  struct doc_iterator : irs::doc_iterator {
    doc_iterator() {
      attrs.emplace(doc);
    }
    virtual irs::doc_id_t value() const override {
      // prevent iterator to filter out
      return irs::type_limits<irs::type_t::doc_id_t>::invalid();
    }
    virtual bool next() override { return false; }
    virtual irs::doc_id_t seek(irs::doc_id_t) override {
      // prevent iterator to filter out
      return irs::type_limits<irs::type_t::doc_id_t>::invalid();
    }
    virtual const irs::attribute_view& attributes() const noexcept override {
      return attrs;
    }

    irs::document doc;
    irs::attribute_view attrs;
  }; // doc_iterator

  struct prepared: public irs::filter::prepared {
    virtual irs::doc_iterator::ptr execute(
      const irs::sub_reader&,
      const irs::order::prepared&,
      const irs::attribute_view&
    ) const override {
      return irs::doc_iterator::make<unestimated::doc_iterator>();
    }
  }; // prepared

  virtual filter::prepared::ptr prepare(
      const irs::index_reader&,
      const irs::order::prepared&,
      irs::boost_t,
      const irs::attribute_view& ) const override {
    return filter::prepared::make<unestimated::prepared>();
  }

  DECLARE_FILTER_TYPE();
  DECLARE_FACTORY();

  unestimated() : filter(unestimated::type()) {}
}; // unestimated

DEFINE_FILTER_TYPE(unestimated)
DEFINE_FACTORY_DEFAULT(unestimated)

struct estimated: public irs::filter {
  struct doc_iterator : irs::doc_iterator {
    doc_iterator(irs::cost::cost_t est, bool* evaluated) {
      cost.rule([est, evaluated]() {
        *evaluated = true;
        return est;
      });
      attrs.emplace(cost);
      attrs.emplace(doc);
    }
    virtual irs::doc_id_t value() const override {
      // prevent iterator to filter out
      return irs::type_limits<irs::type_t::doc_id_t>::invalid();
    }
    virtual bool next() override { return false; }
    virtual irs::doc_id_t seek(irs::doc_id_t) override {
      // prevent iterator to filter out
      return irs::type_limits<irs::type_t::doc_id_t>::invalid();
    }
    virtual const irs::attribute_view& attributes() const noexcept override {
      return attrs;
    }

    irs::document doc;
    irs::cost cost;
    irs::attribute_view attrs;
  }; // doc_iterator

  struct prepared: public irs::filter::prepared {
    explicit prepared(irs::cost::cost_t est,bool* evaluated)
      : evaluated(evaluated), est(est) {
    }

    virtual irs::doc_iterator::ptr execute(
      const irs::sub_reader&,
      const irs::order::prepared&,
      const irs::attribute_view&
    ) const override {
      return irs::doc_iterator::make<estimated::doc_iterator>(
        est, evaluated
      );
    }

    bool* evaluated;
    irs::cost::cost_t est;
  }; // prepared
  
  virtual filter::prepared::ptr prepare(
      const irs::index_reader&,
      const irs::order::prepared&,
      irs::boost_t,
      const irs::attribute_view&) const override {
    return filter::prepared::make<estimated::prepared>(est,&evaluated);
  }

  DECLARE_FILTER_TYPE();
  DECLARE_FACTORY();

  explicit estimated()
    : filter(estimated::type()) {
  }

  mutable bool evaluated = false;
  irs::cost::cost_t est{};
}; // estimated

DEFINE_FILTER_TYPE(estimated)
DEFINE_FACTORY_DEFAULT(estimated)

NS_END // detail

TEST( boolean_query_estimation, or ) {
  // estimated subqueries
  {
    irs::Or root;
    root.add<detail::estimated>().est = 100;
    root.add<detail::estimated>().est = 320;
    root.add<detail::estimated>().est = 10;
    root.add<detail::estimated>().est = 1;
    root.add<detail::estimated>().est = 100;

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    auto docs = prep->execute(irs::sub_reader::empty());

    // check that subqueries were not estimated
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      ASSERT_FALSE(it.safe_as<detail::estimated>()->evaluated);
    }

    ASSERT_EQ(531, irs::cost::extract(docs->attributes()));

    // check that subqueries were estimated
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      ASSERT_TRUE(it.safe_as<detail::estimated>()->evaluated);
    }
  }

  // unestimated subqueries
  {
    irs::Or root;
    root.add<detail::unestimated>();
    root.add<detail::unestimated>();
    root.add<detail::unestimated>();
    root.add<detail::unestimated>();

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );
   
    auto docs = prep->execute(irs::sub_reader::empty());
    ASSERT_EQ(0, irs::cost::extract(docs->attributes()));
  }

   // estimated/unestimated subqueries
  {
    irs::Or root;
    root.add<detail::estimated>().est = 100;
    root.add<detail::estimated>().est = 320;
    root.add<detail::unestimated>();
    root.add<detail::estimated>().est = 10;
    root.add<detail::unestimated>();
    root.add<detail::estimated>().est = 1;
    root.add<detail::estimated>().est = 100;
    root.add<detail::unestimated>();

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    auto docs = prep->execute(irs::sub_reader::empty());

    /* check that subqueries were not estimated */
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      auto est_query = it.safe_as<detail::estimated>();
      if (est_query) {
        ASSERT_FALSE(est_query->evaluated);
      }
    }

    ASSERT_EQ(531, irs::cost::extract(docs->attributes()));

    /* check that subqueries were estimated */
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      auto est_query = it.safe_as<detail::estimated>();
      if (est_query) {
        ASSERT_TRUE(est_query->evaluated);
      }
    }
  }

  // estimated/unestimated/negative subqueries
  {
    irs::Or root;
    root.add<detail::estimated>().est = 100;
    root.add<detail::estimated>().est = 320;
    root.add<irs::Not>().filter<detail::estimated>().est = 3;
    root.add<detail::unestimated>();
    root.add<detail::estimated>().est = 10;
    root.add<detail::unestimated>();
    root.add<detail::estimated>().est = 7;
    root.add<detail::estimated>().est = 100;
    root.add<irs::Not>().filter<detail::unestimated>();
    root.add<irs::Not>().filter<detail::estimated>().est = 0;
    root.add<detail::unestimated>();

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    auto docs = prep->execute(irs::sub_reader::empty());

    // check that subqueries were not estimated
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      auto est_query = it.safe_as<detail::estimated>();
      if (est_query) {
        ASSERT_FALSE(est_query->evaluated);
      }
    }

    ASSERT_EQ(537, irs::cost::extract(docs->attributes()));

    // check that subqueries were estimated
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      auto est_query = it.safe_as<detail::estimated>();
      if (est_query) {
        ASSERT_TRUE(est_query->evaluated);
      }
    }
  }

  // empty case
  {
    irs::Or root;

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    auto docs = prep->execute(irs::sub_reader::empty());
    ASSERT_EQ(0, irs::cost::extract(docs->attributes()));
  }
}

TEST( boolean_query_estimation, and ) {
  // estimated subqueries
  {
    irs::And root;
    root.add<detail::estimated>().est = 100;
    root.add<detail::estimated>().est = 320;
    root.add<detail::estimated>().est = 10;
    root.add<detail::estimated>().est = 1;
    root.add<detail::estimated>().est = 100;

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );
    
    auto docs = prep->execute(irs::sub_reader::empty());

    // check that subqueries were estimated
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      auto est_query = it.safe_as<detail::estimated>();
      if (est_query) {
        ASSERT_TRUE(est_query->evaluated);
      }
    }

    ASSERT_EQ(1, irs::cost::extract(docs->attributes()));
  }

  // unestimated subqueries
  {
    irs::And root;
    root.add<detail::unestimated>();
    root.add<detail::unestimated>();
    root.add<detail::unestimated>();
    root.add<detail::unestimated>();

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );
    
    auto docs = prep->execute(irs::sub_reader::empty());

    // check that subqueries were estimated
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      auto est_query = it.safe_as<detail::estimated>();
      if (est_query) {
        ASSERT_TRUE(est_query->evaluated);
      }
    }

    ASSERT_EQ(
      decltype(irs::cost::MAX)(irs::cost::MAX),
      irs::cost::extract(docs->attributes())
    );
  }

  // estimated/unestimated subqueries
  {
    irs::And root;
    root.add<detail::estimated>().est = 100;
    root.add<detail::estimated>().est = 320;
    root.add<detail::unestimated>();
    root.add<detail::estimated>().est = 10;
    root.add<detail::unestimated>();
    root.add<detail::estimated>().est = 1;
    root.add<detail::estimated>().est = 100;
    root.add<detail::unestimated>();

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    auto docs = prep->execute(irs::sub_reader::empty());

    // check that subqueries were estimated
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      auto est_query = it.safe_as<detail::estimated>();
      if (est_query) {
        ASSERT_TRUE(est_query->evaluated);
      }
    }

    ASSERT_EQ(1, irs::cost::extract(docs->attributes()));
  }

  // estimated/unestimated/negative subqueries
  {
    irs::And root;
    root.add<detail::estimated>().est = 100;
    root.add<detail::estimated>().est = 320;
    root.add<irs::Not>().filter<detail::estimated>().est = 3;
    root.add<detail::unestimated>();
    root.add<detail::estimated>().est = 10;
    root.add<detail::unestimated>();
    root.add<detail::estimated>().est = 7;
    root.add<detail::estimated>().est = 100;
    root.add<irs::Not>().filter<detail::unestimated>();
    root.add<irs::Not>().filter<detail::estimated>().est = 0;
    root.add<detail::unestimated>();

    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    auto docs = prep->execute(irs::sub_reader::empty());

    // check that subqueries were estimated
    for(auto it = root.begin(), end = root.end(); it != end; ++it) {
      auto est_query = it.safe_as<detail::estimated>();
      if (est_query) {
        ASSERT_TRUE(est_query->evaluated);
      }
    }

    ASSERT_EQ(7, irs::cost::extract(docs->attributes()));
  }

  // empty case
  {
    irs::And root;
    auto prep = root.prepare(
      irs::sub_reader::empty(),
      irs::order::prepared::unordered()
    );

    auto docs = prep->execute(irs::sub_reader::empty());
    ASSERT_EQ(0, irs::cost::extract(docs->attributes()));
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                       basic disjunction (iterator0 OR iterator1)
// ----------------------------------------------------------------------------

TEST(basic_disjunction, next) {
  typedef irs::basic_disjunction<irs::doc_iterator::ptr> disjunction;
  // simple case
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{ 1, 5, 6, 12, 29 };
    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
      );

      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));

      ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
        ASSERT_EQ(it.value(), doc->value);
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }

    ASSERT_EQ( expected, result );
  }

  // basic case : single dataset
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{};
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
        ASSERT_EQ(it.value(), doc->value);
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }

    ASSERT_EQ( first, result );
  }

  // basic case : single dataset
  {
    std::vector<irs::doc_id_t> first{};
    std::vector<irs::doc_id_t> last{ 1, 5, 6, 12, 29 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
        ASSERT_EQ(it.value(), doc->value);
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( last, result );
  }

  // basic case : same datasets
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
        ASSERT_EQ(it.value(), doc->value);
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( first, result );
  }

  // basic case : single dataset
  {
    std::vector<irs::doc_id_t> first{ 24 };
    std::vector<irs::doc_id_t> last{};
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
        ASSERT_EQ(it.value(), doc->value);
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( first, result );
  }

  // empty
  {
    std::vector<irs::doc_id_t> first{};
    std::vector<irs::doc_id_t> last{};
    std::vector<irs::doc_id_t> expected{};
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
        ASSERT_EQ(it.value(), doc->value);
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }
}

TEST(basic_disjunction_test, seek) {
  typedef irs::basic_disjunction<irs::doc_iterator::ptr> disjunction;
  // simple case
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{ 1, 5, 6, 12, 29 };
    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {1, 1},
        {9, 9},
        {8, 9},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 9},
        {12, 12},
        {8, 12},
        {13, 29},
        {45, 45},
        {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

      disjunction it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
      ASSERT_EQ(it.value(), doc->value);
    }
  }

  // empty datasets
  {
    std::vector<irs::doc_id_t> first{};
    std::vector<irs::doc_id_t> last{};
    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
    );
    ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
      ASSERT_EQ(it.value(), doc->value);
    }
  }

  // NO_MORE_DOCS
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{ 1, 5, 6, 12, 29 };
    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {irs::type_limits<irs::type_t::doc_id_t>::eof(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {9, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {12, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {13, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {45, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
    );
    ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
      ASSERT_EQ(it.value(), doc->value);
    }
  }

  // INVALID_DOC
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{ 1, 5, 6, 12, 29 };
    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {9, 9},
        {12, 12},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 12},
        {45, 45},
        {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
    );
    ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
      ASSERT_EQ(it.value(), doc->value);
    }
  }
}

TEST(basic_disjunction_test, seek_next) {
  typedef irs::basic_disjunction<irs::doc_iterator::ptr> disjunction;

  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{ 1, 5, 6 };

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end()),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end())
    );
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_EQ(5, it.seek(5));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    ASSERT_EQ(11, it.seek(10));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

TEST(basic_disjunction_test, scored_seek_next) {
  typedef irs::basic_disjunction<irs::doc_iterator::ptr> disjunction;
  const irs::byte_type* empty_stats = irs::bytes_ref::EMPTY.c_str();

  // disjunction without order
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    irs::order first_order;
    first_order.add<detail::basic_sort>(false, 1);
    auto prepared_first_order = first_order.prepare();

    std::vector<irs::doc_id_t> last{ 1, 5, 6 };
    irs::order last_order;
    last_order.add<detail::basic_sort>(false, 2);
    auto prepared_last_order = last_order.prepare();

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end(), empty_stats, prepared_first_order),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end(), empty_stats, prepared_last_order)
    );
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // estimation
    ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    ASSERT_EQ(5, it.seek(5));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    ASSERT_EQ(11, it.seek(10));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with order
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    irs::order first_order;
    first_order.add<detail::basic_sort>(false, 1);
    auto prepared_first_order = first_order.prepare();

    std::vector<irs::doc_id_t> last{ 1, 5, 6 };
    irs::order last_order;
    last_order.add<detail::basic_sort>(false, 2);
    auto prepared_last_order = last_order.prepare();

    irs::order order;
    order.add<detail::basic_sort>(false, 0);
    auto prepared_order = order.prepare();

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end(), empty_stats, prepared_first_order),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end(), empty_stats, prepared_last_order),
      prepared_order,
      1 // custom cost
    );
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>().get());
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // estimation
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(3, *reinterpret_cast<const size_t*>(score.c_str())); // 1 + 2
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(3, *reinterpret_cast<const size_t*>(score.c_str())); // 1 + 2
    ASSERT_TRUE(it.next());
    score.evaluate();
    ASSERT_EQ(2, *reinterpret_cast<const size_t*>(score.c_str())); // 2
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_EQ(11, it.seek(10));
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with order, iterators without order
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{ 1, 5, 6 };

    irs::order order;
    order.add<detail::basic_sort>(false, 0);
    auto prepared_order = order.prepare();

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end(), empty_stats),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end(), empty_stats),
      prepared_order
    );
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>());
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // estimation
    ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_TRUE(it.next());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_EQ(11, it.seek(10));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with order, first iterator with order
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    irs::order first_order;
    first_order.add<detail::basic_sort>(false, 1);
    auto prepared_first_order = first_order.prepare();

    std::vector<irs::doc_id_t> last{ 1, 5, 6 };

    irs::order order;
    order.add<detail::basic_sort>(false, 0);
    auto prepared_order = order.prepare();

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end(), empty_stats, prepared_first_order),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end(), empty_stats),
      prepared_order
    );
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>().get());
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // estimation
    ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_EQ(11, it.seek(10));
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with order, last iterator with order
  {
    std::vector<irs::doc_id_t> first{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> last{ 1, 5, 6 };
    irs::order last_order;
    last_order.add<detail::basic_sort>(false, 1);
    auto prepared_last_order = last_order.prepare();

    irs::order order;
    order.add<detail::basic_sort>(false, 0);
    auto prepared_order = order.prepare();

    disjunction it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(first.begin(), first.end(), empty_stats),
      irs::doc_iterator::make<detail::basic_doc_iterator>(last.begin(), last.end(), empty_stats, prepared_last_order),
      prepared_order
    );
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>().get());
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // estimation
    ASSERT_EQ(first.size() + last.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_EQ(11, it.seek(10));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str()));
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

// ----------------------------------------------------------------------------
// --SECTION--   small disjunction (iterator0 OR iterator1 OR iterator2 OR ...)
// ----------------------------------------------------------------------------

TEST(small_disjunction_test, next) {
  using disjunction = irs::small_disjunction<irs::doc_iterator::ptr>;
  auto sum = [](size_t sum, const std::vector<irs::doc_id_t>& docs) { return sum += docs.size(); };

  // no iterators provided
  {
    disjunction it({});
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(0, irs::cost::extract(it.attributes()));
    ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    ASSERT_FALSE(it.next());
    ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };
    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE(it.next());
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }

    ASSERT_EQ( expected, result );
  }

  // basic case : single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 }
    };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }

    ASSERT_EQ(docs[0], result);
  }

  // basic case : same datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 2, 5, 7, 9, 11, 45 }
    };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(docs[0], result );
  }

  // basic case : single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 24 }
    };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(docs[0], result );
  }

  // empty
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      {}, {}
    };
    std::vector<irs::doc_id_t> expected{};
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_TRUE(!irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1 ,5, 6 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45, 79, 101, 141, 256, 1025, 1101 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1 },
      { 2 },
      { 3 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 3 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
    };

    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(docs.front(), result );
  }

  // same datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 2, 5, 7, 9, 11, 45 }
    };

    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( docs[0], result );
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { }, { }, { }
    };

    std::vector<irs::doc_id_t> expected{ };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }
}

TEST(small_disjunction_test, seek) {
  using disjunction = irs::small_disjunction<irs::doc_iterator::ptr>;
  auto sum = [](size_t sum, const std::vector<irs::doc_id_t>& docs) { return sum += docs.size(); };

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };

    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {1, 1},
        {9, 9},
        {8, 9},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 9},
        {12, 12},
        {8, 12},
        {13, 29},
        {45, 45},
        {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      {}, {}
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // NO_MORE_DOCS
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {irs::type_limits<irs::type_t::doc_id_t>::eof(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {9, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {12, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {13, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {45, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // INVALID_DOC
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };
    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {9, 9},
        {12, 12},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 12},
        {45, 45},
        {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // no iterators provided
  {
    disjunction it({});
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(0, irs::cost::extract(it.attributes()));
    ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.seek(42));
    ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1 ,5, 6 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45, 79, 101, 141, 256, 1025, 1101 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1 },
      { 2 },
      { 3 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 3 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
    };

    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(docs.front(), result );
  }

  // same datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 2, 5, 7, 9, 11, 45 }
    };

    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( docs[0], result );
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { }, { }, { }
    };

    std::vector<irs::doc_id_t> expected{ };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }
}

TEST(small_disjunction_test, seek_next) {
  using disjunction = irs::small_disjunction<irs::doc_iterator::ptr>;
  auto sum = [](size_t sum, const std::vector<irs::doc_id_t>& docs) { return sum += docs.size(); };

  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 }
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_EQ(5, it.seek(5));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    ASSERT_EQ(29, it.seek(27));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

TEST(small_disjunction_test, scored_seek_next) {
  using disjunction = irs::small_disjunction<irs::doc_iterator::ptr>;

  // disjunction without score, sub-iterators with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 2);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6 }, std::move(ord));
    }

    auto res = detail::execute_all<disjunction::doc_iterator_t>(docs);
    disjunction it(std::move(res.first), irs::order::prepared::unordered(), 1); // custom cost
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_EQ(5, it.seek(5));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    ASSERT_EQ(29, it.seek(27));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 2);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<disjunction::doc_iterator_t>(docs);
    disjunction it(std::move(res.first), prepared_order, 1); // custom cost
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(6, *reinterpret_cast<const size_t*>(score.c_str())); // 2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(2, *reinterpret_cast<const size_t*>(score.c_str())); // 2
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators partially with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<disjunction::doc_iterator_t>(docs);
    disjunction it(std::move(res.first), prepared_order, 1); // custom cost
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(4, *reinterpret_cast<const size_t*>(score.c_str())); // 4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); //
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators partially without scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<disjunction::doc_iterator_t>(docs);
    disjunction it(std::move(res.first), prepared_order, 1); // custom cost
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); //
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

// ----------------------------------------------------------------------------
// --SECTION--         disjunction (iterator0 OR iterator1 OR iterator2 OR ...)
// ----------------------------------------------------------------------------

TEST(disjunction_test, next) {
  using disjunction = irs::disjunction<irs::doc_iterator::ptr>;
  auto sum = [](size_t sum, const std::vector<irs::doc_id_t>& docs) { return sum += docs.size(); };

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };
    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE(it.next());
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }

    ASSERT_EQ( expected, result );
  }

  // basic case : single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 }
    };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }

    ASSERT_EQ(docs[0], result);
  }

  // basic case : same datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 2, 5, 7, 9, 11, 45 }
    };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(docs[0], result );
  }

  // basic case : single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 24 }
    };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(docs[0], result );
  }

  // empty
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      {}, {}
    };
    std::vector<irs::doc_id_t> expected{};
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_TRUE(!irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // no iterators provided
  {
    disjunction it({});
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
    ASSERT_EQ(0, irs::cost::extract(it.attributes()));
    ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    ASSERT_FALSE(it.next());
    ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1 ,5, 6 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 9, 11, 12, 29, 45, 79, 101, 141, 256, 1025, 1101 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1 },
      { 2 },
      { 3 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 2, 3 };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
    };

    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(docs.front(), result );
  }

  // same datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 2, 5, 7, 9, 11, 45 }
    };

    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( docs[0], result );
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { }, { }, { }
    };

    std::vector<irs::doc_id_t> expected{ };
    std::vector<irs::doc_id_t> result;
    {
      disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }
}

TEST(disjunction_test, seek) {
  using disjunction = irs::disjunction<irs::doc_iterator::ptr>;
  auto sum = [](size_t sum, const std::vector<irs::doc_id_t>& docs) { return sum += docs.size(); };

  // no iterators provided
  {
    disjunction it({});
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(0, irs::cost::extract(it.attributes()));
    ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.seek(42));
    ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };

    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {1, 1},
        {9, 9},
        {8, 9},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 9},
        {12, 12},
        {8, 12},
        {13, 29},
        {45, 45},
        {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      {}, {}
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // NO_MORE_DOCS
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {irs::type_limits<irs::type_t::doc_id_t>::eof(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {9, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {12, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {13, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {45, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // INVALID_DOC
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };
    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {9, 9},
        {12, 12},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 12},
        {45, 45},
        {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 }
    };

    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {1, 1},
        {9, 9},
        {8, 9},
        {12, 12},
        {13, 29},
        {45, 45},
        {44, 45},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 45},
        {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {1, 1},
        {9, 9},
        {8, 9},
        {13, 29},
        {45, 45},
        {80, 101},
        {513, 1025},
        {2, 1025},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 1025},
        {2001, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      {}, {}, {}, {}
    };
    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // NO_MORE_DOCS
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {irs::type_limits<irs::type_t::doc_id_t>::eof(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {9, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {12, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {13, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {45, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // INVALID_DOC
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {9, 9},
        {12, 12},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 12},
        {45, 45},
        {1201, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }
}

TEST(disjunction_test, seek_next) {
  using disjunction = irs::disjunction<irs::doc_iterator::ptr>;
  auto sum = [](size_t sum, const std::vector<irs::doc_id_t>& docs) { return sum += docs.size(); };

  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 }
    };

    disjunction it(detail::execute_all<disjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(std::accumulate(docs.begin(), docs.end(), size_t(0), sum), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_EQ(5, it.seek(5));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    ASSERT_EQ(29, it.seek(27));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

TEST(disjunction_test, scored_seek_next) {
  using disjunction = irs::disjunction<irs::doc_iterator::ptr>;

  // disjunction without score, sub-iterators with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 2);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6 }, std::move(ord));
    }

    auto res = detail::execute_all<disjunction::doc_iterator_t>(docs);
    disjunction it(std::move(res.first), irs::order::prepared::unordered(), 1); // custom cost
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_EQ(5, it.seek(5));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    ASSERT_EQ(29, it.seek(27));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 2);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<disjunction::doc_iterator_t>(docs);
    disjunction it(std::move(res.first), prepared_order, 1); // custom cost
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(6, *reinterpret_cast<const size_t*>(score.c_str())); // 2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(2, *reinterpret_cast<const size_t*>(score.c_str())); // 2
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators with scores partially
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<disjunction::doc_iterator_t>(docs);
    disjunction it(std::move(res.first), prepared_order, 1); // custom cost
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(4, *reinterpret_cast<const size_t*>(score.c_str())); // 4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); //
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(1, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators without scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<disjunction::doc_iterator_t>(docs);
    disjunction it(std::move(res.first), prepared_order, 1); // custom cost
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(1, irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(7, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); //
    ASSERT_TRUE(it.next());
    ASSERT_EQ(45, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

// ----------------------------------------------------------------------------
// --SECTION--  Minimum match count: iterator0 OR iterator1 OR iterator2 OR ...
// ----------------------------------------------------------------------------

TEST(min_match_disjunction_test, next) {
  using disjunction = irs::min_match_disjunction<irs::doc_iterator::ptr>;
  // single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
        { 1, 2, 5, 7, 9, 11, 45 },
    };

    {
      const size_t min_match_count = 0;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    {
      const size_t min_match_count = 1;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    {
      const size_t min_match_count = 2;
      std::vector<irs::doc_id_t> expected{};
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    {
      const size_t min_match_count = 6;
      std::vector<irs::doc_id_t> expected{};
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    {
      const size_t min_match_count = irs::integer_traits<size_t>::const_max;
      std::vector<irs::doc_id_t> expected{};
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));

        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
        { 1, 2, 5, 7, 9, 11, 45 },
        { 7, 15, 26, 212, 239 },
        { 1001,4001,5001 },
        { 10, 101, 490, 713, 1201, 2801 },
    };

    {
      const size_t min_match_count = 0;
      std::vector<irs::doc_id_t> expected = detail::union_all(docs);
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    {
      const size_t min_match_count = 1;
      std::vector<irs::doc_id_t> expected = detail::union_all(docs);
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    {
      const size_t min_match_count = 2;
      std::vector<irs::doc_id_t> expected{ 7 };
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    {
      const size_t min_match_count = 3;
      std::vector<irs::doc_id_t> expected;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = 4;
      std::vector<irs::doc_id_t> expected;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = 5;
      std::vector<irs::doc_id_t> expected{};
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = irs::integer_traits<size_t>::const_max;
      std::vector<irs::doc_id_t> expected{};
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
        { 1, 2, 5, 7, 9, 11, 45 },
        { 1, 5, 6, 12, 29 },
        { 1 ,5, 6 },
        { 1, 2, 5, 8, 13, 29 },
    };

    {
      const size_t min_match_count = 0;
      std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 8, 9, 11, 12, 13, 29, 45 };
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    {
      const size_t min_match_count = 1;
      std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 7, 8, 9, 11, 12, 13, 29, 45 };
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    {
      const size_t min_match_count = 2;
      std::vector<irs::doc_id_t> expected{ 1, 2, 5, 6, 29 };
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    {
      const size_t min_match_count = 3;
      std::vector<irs::doc_id_t> expected{ 1, 5 };
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = 4;
      std::vector<irs::doc_id_t> expected{ 1, 5 };
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = 5;
      std::vector<irs::doc_id_t> expected{ 1, 5 };
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = irs::integer_traits<size_t>::const_max;
      std::vector<irs::doc_id_t> expected{ 1, 5 };
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( expected, result );
    }
  }

  // same datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
        { 1, 2, 5, 7, 9, 11, 45 },
        { 1, 2, 5, 7, 9, 11, 45 },
        { 1, 2, 5, 7, 9, 11, 45 },
        { 1, 2, 5, 7, 9, 11, 45 },
    };

    // equals to disjunction
    {
      const size_t min_match_count = 0;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    // equals to disjunction
    {
      const size_t min_match_count = 1;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    {
      const size_t min_match_count = 2;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    {
      const size_t min_match_count = 3;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = 4;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = 5;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }

    // equals to conjunction
    {
      const size_t min_match_count = irs::integer_traits<size_t>::const_max;
      std::vector<irs::doc_id_t> result;
      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
            min_match_count
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
        for ( ; it.next(); ) {
          result.push_back( it.value() );
        }
        ASSERT_FALSE( it.next() );
        ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
      }
      ASSERT_EQ( docs.front(), result );
    }
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
        { }, { }, { }
    };

    std::vector<irs::doc_id_t> expected{ };
    {
      std::vector<irs::doc_id_t> expected{};
      {
        std::vector<irs::doc_id_t> result;
        {
          disjunction it(
              detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 0
          );
          auto& doc = it.attributes().get<irs::document>();
          ASSERT_TRUE(bool(doc));
          ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
          for ( ; it.next(); ) {
            result.push_back( it.value() );
          }
          ASSERT_FALSE( it.next() );
          ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
        }
        ASSERT_EQ( docs.front(), result );
      }

      {
        std::vector<irs::doc_id_t> result;
        {
          disjunction it(
              detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 1
          );
          auto& doc = it.attributes().get<irs::document>();
          ASSERT_TRUE(bool(doc));
          ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
          for ( ; it.next(); ) {
            result.push_back( it.value() );
          }
          ASSERT_FALSE( it.next() );
          ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
        }
        ASSERT_EQ( docs.front(), result );
      }

      {
        std::vector<irs::doc_id_t> result;
        {
          disjunction it(
              detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), irs::integer_traits<size_t>::const_max
          );
          auto& doc = it.attributes().get<irs::document>();
          ASSERT_TRUE(bool(doc));
          ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
          for ( ; it.next(); ) {
            result.push_back( it.value() );
          }
          ASSERT_FALSE( it.next() );
          ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
        }
        ASSERT_EQ( docs.front(), result );
      }
    }
  }
}

TEST(min_match_disjunction_test, seek) {
  using disjunction = irs::min_match_disjunction<irs::doc_iterator::ptr>;

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 29, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6, 12 }
    };

    // equals to disjunction
    {
      const size_t min_match_count = 0;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {9, 9},
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 9},
          {12, 12},
          {11, 12},
          {13, 29},
          {45, 45},
          {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
        detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
        min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    // equals to disjunction
    {
      const size_t min_match_count = 1;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {9, 9},
          {8, 9},
          {12, 12},
          {13, 29},
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 29},
          {45, 45},
          {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    {
      const size_t min_match_count = 2;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {6, 6},
          {4, 6},
          {7, 12},
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 12},
          {29, 29},
          {45, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    // equals to conjunction
    {
      const size_t min_match_count = 3;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {6, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    // equals to conjunction
    {
      const size_t min_match_count = irs::integer_traits<size_t>::const_max;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {6, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45, 79, 101 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    // equals to disjunction
    {
      const size_t min_match_count = 0;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {9, 9},
          {8, 9},
          {13, 29},
          {45, 45},
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 45},
          {80, 101},
          {513, 1025},
          {2001, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    // equals to disjunction
    {
      const size_t min_match_count = 1;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {9, 9},
          {8, 9},
          {13, 29},
          {45, 45},
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 45},
          {80, 101},
          {513, 1025},
          {2001, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    {
      const size_t min_match_count = 2;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {6, 6},
          {2, 6},
          {13, 79},
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 79},
          {101, 101},
          {513, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
        ASSERT_EQ(it.value(), doc->value);
      }
    }

    {
      const size_t min_match_count = 3;
      std::vector<detail::seek_doc> expected{
          {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
          {1, 1},
          {6, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
        ASSERT_EQ(it.value(), doc->value);
      }
    }

    // equals to conjunction
    {
      const size_t min_match_count = irs::integer_traits<size_t>::const_max;
      std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {1, irs::type_limits<irs::type_t::doc_id_t>::eof()},
        {6, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs),
          min_match_count
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
        ASSERT_EQ(it.value(), doc->value);
      }
    }
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      {}, {}, {}, {}
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    {
      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 0
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    {
      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 1
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    {
      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), irs::integer_traits<size_t>::const_max
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }
  }

  // NO_MORE_DOCS
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {irs::type_limits<irs::type_t::doc_id_t>::eof(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {9, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {12, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {13, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {45, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    {
      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 0
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    {
      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 1
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    {
      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 2
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }

    {
      disjunction it(
          detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), irs::integer_traits<size_t>::const_max
      );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      for (const auto& target : expected) {
        ASSERT_EQ(target.expected, it.seek(target.target));
      }
    }
  }

  // INVALID_DOC
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6 },
      { 256 },
      { 11, 79, 101, 141, 1025, 1101 }
    };

    // equals to disjunction
    {
      std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {9, 9},
        {12, 12},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 12},
        {45, 45},
        {44, 45},
        {1201, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 0
        );
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
        for (const auto& target : expected) {
          ASSERT_EQ(target.expected, it.seek(target.target));
        }
      }

      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 1
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        for (const auto& target : expected) {
          ASSERT_EQ(target.expected, it.seek(target.target));
        }
      }
    }

    {
      std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {6, 6},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 6},
        {12, irs::type_limits<irs::type_t::doc_id_t>::eof()}
      };

      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 2
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        for (const auto& target : expected) {
          ASSERT_EQ(target.expected, it.seek(target.target));
        }
      }
    }

    {
      std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      };

      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 3
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        for (const auto& target : expected) {
          ASSERT_EQ(target.expected, it.seek(target.target));
        }
      }
    }

    // equals to conjuction
    {
      std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      };

      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 5
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        for (const auto& target : expected) {
          ASSERT_EQ(target.expected, it.seek(target.target));
        }
      }

      {
        disjunction it(
            detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), irs::integer_traits<size_t>::const_max
        );
        auto& doc = it.attributes().get<irs::document>();
        ASSERT_TRUE(bool(doc));
        for (const auto& target : expected) {
          ASSERT_EQ(target.expected, it.seek(target.target));
        }
      }
    }
  }
}

TEST(min_match_disjunction_test, seek_next) {
  using disjunction = irs::min_match_disjunction<irs::doc_iterator::ptr>;

  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 6, 9, 29 }
    };

    disjunction it(detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs), 2);
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());

    ASSERT_EQ(5, it.seek(5));
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_TRUE(it.next());
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_EQ(6, it.value());
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_TRUE(it.next());
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_EQ(9, it.value());
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_EQ(29, it.seek(27));
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_FALSE(it.next());
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_FALSE(it.next());
    ASSERT_EQ(it.value(), doc->value);
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_EQ(it.value(), doc->value);
  }
}

TEST(min_match_disjunction_test, scored_seek_next) {
  using disjunction = irs::min_match_disjunction<irs::doc_iterator::ptr>;

  // disjunction without score, sub-iterators with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 2);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 9, 29 }, std::move(ord));
    }

    auto res = detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs);
    disjunction it(std::move(res.first), 2, irs::order::prepared::unordered());
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(docs[0].first.size() + docs[1].first.size() + docs[2].first.size(),  irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_EQ(5, it.seek(5));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(9, it.value());
    ASSERT_EQ(29, it.seek(27));
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 2);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 9, 29 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs);
    disjunction it(std::move(res.first), 2, prepared_order);
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(docs[0].first.size() + docs[1].first.size() + docs[2].first.size(),  irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(6, *reinterpret_cast<const size_t*>(score.c_str())); // 2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(9, it.value());
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(6, *reinterpret_cast<const size_t*>(score.c_str())); // 2+4
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators with scores partially
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 9, 29 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs);
    disjunction it(std::move(res.first), 2, prepared_order);
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(docs[0].first.size()+docs[1].first.size()+docs[2].first.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(4, *reinterpret_cast<const size_t*>(score.c_str())); // 2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(9, it.value());
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(4, *reinterpret_cast<const size_t*>(score.c_str())); // 2+4
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // disjunction with score, sub-iterators without scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 5, 7, 9, 11, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 12, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 5, 6, 9, 29 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<irs::min_match_disjunction<irs::doc_iterator::ptr>::cost_iterator_adapter>(docs);
    disjunction it(std::move(res.first), 2, prepared_order);
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(docs[0].first.size() + docs[1].first.size() + docs[2].first.size(),  irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(5, it.seek(5));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(6, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(9, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+4
    ASSERT_EQ(29, it.seek(27));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 2+4
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                    iterator0 AND iterator1 AND iterator2 AND ... 
// ----------------------------------------------------------------------------

TEST(conjunction_test, next) {
  using conjunction = irs::conjunction<irs::doc_iterator::ptr>;
  auto shortest = [](const std::vector<irs::doc_id_t>& lhs, const std::vector<irs::doc_id_t>& rhs) {
    return lhs.size() < rhs.size();
  };

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 5, 6 },
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 },
      { 1, 5, 79, 101, 141, 1025, 1101 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 5 };
    std::vector<irs::doc_id_t> result;
    {
      conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
      for ( ; it.next(); ) {
        result.push_back( it.value() );
        ASSERT_EQ(it.value(), doc->value);
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // not optimal case, first is the longest
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 },
      { 1, 5, 11, 21, 27, 31 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 5, 11, 21, 27, 31 };
    std::vector<irs::doc_id_t> result;
    {
      conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // simple case
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 5, 11, 21, 27, 31 },
      { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 },
    };

    std::vector<irs::doc_id_t> expected{ 1, 5, 11, 21, 27, 31 };
    std::vector<irs::doc_id_t> result;
    {
      conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // not optimal case, first is the longest
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 5, 79, 101, 141, 1025, 1101 },
      { 1, 5, 6 },
      { 1, 2, 5, 7, 9, 11, 45 },
      { 1, 5, 6, 12, 29 }
    };

    std::vector<irs::doc_id_t> expected{ 1, 5 };
    std::vector<irs::doc_id_t> result;
    {
      conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // same datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 5, 79, 101, 141, 1025, 1101 },
      { 1, 5, 79, 101, 141, 1025, 1101 },
      { 1, 5, 79, 101, 141, 1025, 1101 },
      { 1, 5, 79, 101, 141, 1025, 1101 }
    };

    std::vector<irs::doc_id_t> result;
    {
      conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( docs.front(), result );
  }

  // single dataset
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 5, 79, 101, 141, 1025, 1101 }
    };

    std::vector<irs::doc_id_t> result;
    {
      conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
      for ( ; it.next(); ) {
        result.push_back(it.value());
      }
      ASSERT_FALSE(it.next());
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(docs.front(), result);
  }

  // empty intersection
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 5, 6 },
      { 1, 2, 3, 7, 9, 11, 45 },
      { 3, 5, 6, 12, 29 },
      { 1, 5, 79, 101, 141, 1025, 1101 }
    };

    std::vector<irs::doc_id_t> expected{ };
    std::vector<irs::doc_id_t> result;
    {
      conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      {}, {}, {}, {}
    };

    std::vector<irs::doc_id_t> expected{};
    std::vector<irs::doc_id_t> result;
    {
      conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
      auto& doc = it.attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }
}

TEST(conjunction_test, seek) {
  using conjunction = irs::conjunction<irs::doc_iterator::ptr>;
  auto shortest = [](const std::vector<irs::doc_id_t>& lhs, const std::vector<irs::doc_id_t>& rhs) {
    return lhs.size() < rhs.size();
  };

  // simple case
  {
    // 1 6 28 45 99 256
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 5, 6, 45, 77, 99, 256, 988 },
      { 1, 2, 5, 6, 7, 9, 11, 28, 45, 99, 256 },
      { 1, 5, 6, 12, 28, 45, 99, 124, 256, 553 },
      { 1, 6, 11, 29, 45, 99, 141, 256, 1025, 1101 }
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {1, 1},
      {6, 6},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 6},
      {29, 45},
      {46, 99},
      {68, 99},
      {256, 256},
      {257, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
      ASSERT_EQ(it.value(), doc->value);
    }
  }

  // not optimal, first is the longest
  {
    // 1 6 28 45 99 256
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 6, 11, 29, 45, 99, 141, 256, 1025, 1101 },
      { 1, 2, 5, 6, 7, 9, 11, 28, 45, 99, 256 },
      { 1, 5, 6, 12, 29, 45, 99, 124, 256, 553 },
      { 1, 5, 6, 45, 77, 99, 256, 988 }
    };

    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {1, 1},
        {6, 6},
        {29, 45},
        {44, 45},
        {46, 99},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 99},
        {256, 256},
        {257, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    conjunction it(detail::execute_all<conjunction ::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // empty datasets
  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      {}, {}, {}, {}
    };
    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // NO_MORE_DOCS
  {
    // 1 6 28 45 99 256
    std::vector<std::vector<irs::doc_id_t>> docs{
        {1, 6, 11, 29, 45, 99, 141, 256, 1025, 1101},
        {1, 2, 5,  6,  7,  9,  11,  28,  45, 99, 256},
        {1, 5, 6,  12, 29, 45, 99,  124, 256,  553},
        {1, 5, 6,  45, 77, 99, 256, 988}
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {irs::type_limits<irs::type_t::doc_id_t>::eof(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {9, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {12, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {13, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {45, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }

  // INVALID_DOC
  {
    // 1 6 28 45 99 256
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 6, 11, 29, 45, 99, 141, 256, 1025, 1101 },
      { 1, 2, 5, 6, 7, 9, 11, 28, 45, 99, 256 },
      { 1, 5, 6, 12, 29, 45, 99, 124, 256, 553 },
      { 1, 5, 6, 45, 77, 99, 256, 988 }
    };

    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {6, 6},
      {45, 45},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 45},
      {99, 99},
      {257, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };

    conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));
    ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));
    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek(target.target));
    }
  }
}

TEST(conjunction_test, seek_next) {
  using conjunction = irs::conjunction<irs::doc_iterator::ptr>;
  auto shortest = [](const std::vector<irs::doc_id_t>& lhs, const std::vector<irs::doc_id_t>& rhs) {
    return lhs.size() < rhs.size();
  };

  {
    std::vector<std::vector<irs::doc_id_t>> docs{
      { 1, 2, 4, 5, 7, 8, 9, 11, 14, 45 },
      { 1, 4, 5, 6, 8, 12, 14, 29 },
      { 1, 4, 5, 8, 14 }
    };

    conjunction it(detail::execute_all<conjunction::doc_iterator_t>(docs));
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(std::min_element(docs.begin(), docs.end(), shortest)->size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_EQ(4, it.seek(3));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(5, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(8, it.value());
    ASSERT_EQ(14, it.seek(14));
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

TEST(conjunction_test, scored_seek_next) {
  using conjunction = irs::conjunction<irs::doc_iterator::ptr>;

  // conjunction without score, sub-iterators with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 4, 5, 7, 8, 9, 11, 14, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 2);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 4, 5, 6, 8, 12, 14, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 4, 5, 8, 14 }, std::move(ord));
    }

    auto res = detail::execute_all<conjunction::doc_iterator_t>(docs);
    conjunction it(std::move(res.first), irs::order::prepared::unordered());
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_EQ(&irs::score::no_score(), &score);
    ASSERT_TRUE(score.empty());

    // cost
    ASSERT_EQ(docs[2].first.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_EQ(4, it.seek(3));
    ASSERT_TRUE(it.next());
    ASSERT_EQ(5, it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(8, it.value());
    ASSERT_EQ(14, it.seek(14));
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // conjunction with score, sub-iterators with scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 4, 5, 7, 8, 9, 11, 14, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 2);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 4, 5, 6, 8, 12, 14, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 4, 5, 8, 14 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<conjunction::doc_iterator_t>(docs);
    conjunction it(std::move(res.first), prepared_order);
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(docs[2].first.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(4, it.seek(3));
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(5, it.value());
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(8, it.value());
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(14, it.seek(14));
    score.evaluate();
    ASSERT_EQ(7, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // conjunction with score, sub-iterators with scores partially
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 1);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 4, 5, 7, 8, 9, 11, 14, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 4, 5, 6, 8, 12, 14, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      ord.add<detail::basic_sort>(false, 4);
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 4, 5, 8, 14 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<conjunction::doc_iterator_t>(docs);
    conjunction it(std::move(res.first), prepared_order);
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(docs[2].first.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(4, it.seek(3));
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(5, it.value());
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(8, it.value());
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(14, it.seek(14));
    score.evaluate();
    ASSERT_EQ(5, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }

  // conjunction with score, sub-iterators without scores
  {
    std::vector<std::pair<std::vector<irs::doc_id_t>, irs::order>> docs;
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 2, 4, 5, 7, 8, 9, 11, 14, 45 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 4, 5, 6, 8, 12, 14, 29 }, std::move(ord));
    }
    {
      irs::order ord;
      docs.emplace_back(std::vector<irs::doc_id_t>{ 1, 4, 5, 8, 14 }, std::move(ord));
    }

    irs::order ord;
    ord.add<detail::basic_sort>(false, std::numeric_limits<size_t>::max());
    auto prepared_order = ord.prepare();

    auto res = detail::execute_all<conjunction::doc_iterator_t>(docs);
    conjunction it(std::move(res.first), prepared_order);
    auto& doc = it.attributes().get<irs::document>();
    ASSERT_TRUE(bool(doc));

    // score
    ASSERT_NE(nullptr, it.attributes().get<irs::score>()); // no order set
    auto& score = irs::score::extract(it.attributes());
    ASSERT_NE(&irs::score::no_score(), &score);
    ASSERT_FALSE(score.empty());

    // cost
    ASSERT_EQ(docs[2].first.size(), irs::cost::extract(it.attributes()));

    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(), it.value());
    ASSERT_TRUE(it.next());
    ASSERT_EQ(1, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(4, it.seek(3));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(5, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_TRUE(it.next());
    ASSERT_EQ(8, it.value());
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_EQ(14, it.seek(14));
    score.evaluate();
    ASSERT_EQ(0, *reinterpret_cast<const size_t*>(score.c_str())); // 1+2+4
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
    ASSERT_FALSE(it.next());
    ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::eof(), it.value());
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                                      iterator0 AND NOT iterator1
// ----------------------------------------------------------------------------

TEST(exclusion_test, next) {
  // simple case
  {
    std::vector<irs::doc_id_t> included{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> excluded{ 1, 5, 6, 12, 29 };
    std::vector<irs::doc_id_t> expected{ 2, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> result;
    {
      irs::exclusion it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
      );

      // score
      ASSERT_EQ(nullptr, it.attributes().get<irs::score>()); // no order set
      auto& score = irs::score::extract(it.attributes());
      ASSERT_EQ(&irs::score::no_score(), &score);
      ASSERT_TRUE(score.empty());

      // cost
      ASSERT_EQ(included.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // basic case: single dataset
  {
    std::vector<irs::doc_id_t> included{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> excluded{};
    std::vector<irs::doc_id_t> result;
    {
      irs::exclusion it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
      );
      ASSERT_EQ(included.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(included, result);
  }

  // basic case: single dataset
  {
    std::vector<irs::doc_id_t> included{};
    std::vector<irs::doc_id_t> excluded{ 1, 5, 6, 12, 29 };
    std::vector<irs::doc_id_t> result;
    {
      irs::exclusion it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
      );
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ(included, result);
  }

  // basic case: same datasets
  {
    std::vector<irs::doc_id_t> included{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> excluded{ 1, 2, 5, 7, 9, 11, 45 };
    std::vector<irs::doc_id_t> expected{};
    std::vector<irs::doc_id_t> result;
    {
      irs::exclusion it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
      );
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }

  // basic case: single dataset
  {
    std::vector<irs::doc_id_t> included{ 24 };
    std::vector<irs::doc_id_t> excluded{};
    std::vector<irs::doc_id_t> result;
    {
      irs::exclusion it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
      );
      ASSERT_EQ(included.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( included, result );
  }

  // empty
  {
    std::vector<irs::doc_id_t> included{};
    std::vector<irs::doc_id_t> excluded{};
    std::vector<irs::doc_id_t> expected{};
    std::vector<irs::doc_id_t> result;
    {
      irs::exclusion it(
        irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
        irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
      );
      ASSERT_EQ(included.size(), irs::cost::extract(it.attributes()));
      ASSERT_FALSE(irs::type_limits<irs::type_t::doc_id_t>::valid(it.value()));
      for ( ; it.next(); ) {
        result.push_back( it.value() );
      }
      ASSERT_FALSE( it.next() );
      ASSERT_TRUE(irs::type_limits<irs::type_t::doc_id_t>::eof(it.value()));
    }
    ASSERT_EQ( expected, result );
  }
}

TEST(exclusion_test, seek) {
  // simple case
  {
    // 2, 7, 9, 11, 45
    std::vector<irs::doc_id_t> included{ 1, 2, 5, 7, 9, 11, 29, 45 };
    std::vector<irs::doc_id_t> excluded{ 1, 5, 6, 12, 29 };
    std::vector<detail::seek_doc> expected{
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
        {1, 2},
        {5, 7},
        {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 7},
        {9, 9},
        {45, 45},
        {43, 45},
        {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };
    irs::exclusion it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
      irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
    );
    ASSERT_EQ(included.size(), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // empty datasets
  {
    std::vector<irs::doc_id_t> included{};
    std::vector<irs::doc_id_t> excluded{};
    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {6, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };
    irs::exclusion it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
      irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
    );
    ASSERT_EQ(included.size(), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // NO_MORE_DOCS
  {
    // 2, 7, 9, 11, 45
    std::vector<irs::doc_id_t> included{ 1, 2, 5, 7, 9, 11, 29, 45 };
    std::vector<irs::doc_id_t> excluded{ 1, 5, 6, 12, 29 };
    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {irs::type_limits<irs::type_t::doc_id_t>::eof(), irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {9, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {12, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {13, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {45, irs::type_limits<irs::type_t::doc_id_t>::eof()},
      {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };
    irs::exclusion it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
      irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
    );
    ASSERT_EQ(included.size(), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }

  // INVALID_DOC
  {
    // 2, 7, 9, 11, 45
    std::vector<irs::doc_id_t> included{ 1, 2, 5, 7, 9, 11, 29, 45 };
    std::vector<irs::doc_id_t> excluded{ 1, 5, 6, 12, 29 };
    std::vector<detail::seek_doc> expected{
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), irs::type_limits<irs::type_t::doc_id_t>::invalid()},
      {7, 7},
      {11, 11},
      {irs::type_limits<irs::type_t::doc_id_t>::invalid(), 11},
      {45, 45},
      {57, irs::type_limits<irs::type_t::doc_id_t>::eof()}
    };
    irs::exclusion it(
      irs::doc_iterator::make<detail::basic_doc_iterator>(included.begin(), included.end()),
      irs::doc_iterator::make<detail::basic_doc_iterator>(excluded.begin(), excluded.end())
    );
    ASSERT_EQ(included.size(), irs::cost::extract(it.attributes()));

    for (const auto& target : expected) {
      ASSERT_EQ(target.expected, it.seek( target.target ));
    }
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                                                Boolean test case 
// ----------------------------------------------------------------------------

class boolean_filter_test_case : public filter_test_case_base { };

TEST_P(boolean_filter_test_case, or_sequential_multiple_segments) {
  // populate index
  {
    tests::json_doc_generator gen(
      resource("simple_sequential.json"),
      &tests::generic_json_field_factory);

    tests::document const *doc1 = gen.next();
    tests::document const *doc2 = gen.next();
    tests::document const *doc3 = gen.next();
    tests::document const *doc4 = gen.next();
    tests::document const *doc5 = gen.next();
    tests::document const *doc6 = gen.next();
    tests::document const *doc7 = gen.next();
    tests::document const *doc8 = gen.next();
    tests::document const *doc9 = gen.next();

    auto writer = open_writer();

    ASSERT_TRUE(insert(*writer,
      doc1->indexed.begin(), doc1->indexed.end(),
      doc1->stored.begin(), doc1->stored.end()
    )); // A
    ASSERT_TRUE(insert(*writer,
      doc2->indexed.begin(), doc2->indexed.end(),
      doc2->stored.begin(), doc2->stored.end()
    )); // B
    ASSERT_TRUE(insert(*writer,
      doc3->indexed.begin(), doc3->indexed.end(),
      doc3->stored.begin(), doc3->stored.end()
    )); // C
    ASSERT_TRUE(insert(*writer,
      doc4->indexed.begin(), doc4->indexed.end(),
      doc4->stored.begin(), doc4->stored.end()
    )); // D
    writer->commit();
    ASSERT_TRUE(insert(*writer,
      doc5->indexed.begin(), doc5->indexed.end(),
      doc5->stored.begin(), doc5->stored.end()
    )); // E
    ASSERT_TRUE(insert(*writer,
      doc6->indexed.begin(), doc6->indexed.end(),
      doc6->stored.begin(), doc6->stored.end()
    )); // F
    ASSERT_TRUE(insert(*writer,
      doc7->indexed.begin(), doc7->indexed.end(),
      doc7->stored.begin(), doc7->stored.end()
    )); // G
    writer->commit();
    ASSERT_TRUE(insert(*writer,
      doc8->indexed.begin(), doc8->indexed.end(),
      doc8->stored.begin(), doc8->stored.end()
    )); // H
    ASSERT_TRUE(insert(*writer,
      doc9->indexed.begin(), doc9->indexed.end(),
      doc9->stored.begin(), doc9->stored.end()
    )); // I
    writer->commit();
  }

  auto rdr = open_reader();
  {
    irs::Or root;
    root.add<irs::by_term>().field("name").term("B");
    root.add<irs::by_term>().field("name").term("F");
    root.add<irs::by_term>().field("name").term("I");

    auto prep = root.prepare(rdr);
    auto segment = rdr.begin();
    {
      auto docs = prep->execute(*segment);
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(2, docs->value());
      ASSERT_FALSE(docs->next());
    }

    ++segment;
    {
      auto docs = prep->execute(*segment);
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(2, docs->value());
      ASSERT_FALSE(docs->next());
    }

    ++segment;
    {
      auto docs = prep->execute(*segment);
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(2, docs->value());
      ASSERT_FALSE(docs->next());
    }
  }
}

TEST_P(boolean_filter_test_case, or_sequential) {
  // add segment
  {
    tests::json_doc_generator gen(
      resource("simple_sequential.json"),
      &tests::generic_json_field_factory);
    add_segment( gen );
  }

  auto rdr = open_reader();

  // empty query
  {
    check_query(irs::Or(), docs_t{}, rdr);
  }

  // name=V
  {
    irs::Or root;
    root.add<irs::by_term>().field("name").term("V"); // 22

    check_query(root, docs_t{ 22 }, rdr);
  }

  // name=W OR name=Z
  {
    irs::Or root;
    root.add<irs::by_term>().field("name").term("W"); // 23
    root.add<irs::by_term>().field("name").term("C"); // 3

    check_query(root, docs_t{ 3, 23 }, rdr);
  }

  // name=A OR name=Q OR name=Z
  {
    irs::Or root;
    root.add<irs::by_term>().field("name").term("A"); // 1
    root.add<irs::by_term>().field("name").term("Q"); // 17
    root.add<irs::by_term>().field("name").term("Z"); // 26

    check_query(root, docs_t{ 1, 17, 26 }, rdr);
  }

  // name=A OR name=Q OR same!=xyz
  {
    irs::Or root;
    root.add<irs::by_term>().field("name").term("A"); // 1
    root.add<irs::by_term>().field("name").term("Q"); // 17
    root.add<irs::Or>().add<irs::Not>().filter<irs::by_term>().field("same").term("xyz"); // none (not within an OR must be wrapped inside a single-branch OR)

    check_query(root, docs_t{ 1, 17 }, rdr);
  }

  // (name=A OR name=Q) OR same!=xyz
  {
    irs::Or root;
    root.add<irs::by_term>().field("name").term("A"); // 1
    root.add<irs::by_term>().field("name").term("Q"); // 17
    root.add<irs::Or>().add<irs::Not>().filter<irs::by_term>().field("same").term("xyz"); // none (not within an OR must be wrapped inside a single-branch OR)

    check_query(root, docs_t{ 1, 17 }, rdr);
  }

  // name=A OR name=Q OR name=Z OR same=invalid_term OR invalid_field=V
  {
    irs::Or root;
    root.add<irs::by_term>().field("name").term("A"); // 1
    root.add<irs::by_term>().field("name").term("Q"); // 17
    root.add<irs::by_term>().field("name").term("Z"); // 26
    root.add<irs::by_term>().field("same").term("invalid_term");
    root.add<irs::by_term>().field("invalid_field").term("V");

    check_query(root, docs_t{ 1, 17, 26 }, rdr);
  }

  // search : all terms
  {
    irs::Or root;
    root.add<irs::by_term>().field("name").term("A"); // 1
    root.add<irs::by_term>().field("name").term("Q"); // 17
    root.add<irs::by_term>().field("name").term("Z"); // 26
    root.add<irs::by_term>().field("same").term("xyz"); // 1..32
    root.add<irs::by_term>().field("same").term("invalid_term");

    check_query(
      root,
      docs_t{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 },
      rdr
    );
  }

  // search : empty result
  check_query(
    irs::by_term().field( "same" ).term( "invalid_term" ),
    docs_t{},
    rdr
  );
}

TEST_P(boolean_filter_test_case, and_schemas) {
  // write segments
  {
    auto writer = open_writer(irs::OM_CREATE);

    std::vector<doc_generator_base::ptr> gens;

    gens.emplace_back(new tests::json_doc_generator(
      resource("AdventureWorks2014.json"),
      &tests::generic_json_field_factory));
    gens.emplace_back(new tests::json_doc_generator(
      resource("AdventureWorks2014Edges.json"),
      &tests::generic_json_field_factory));
    gens.emplace_back(new tests::json_doc_generator(
      resource("Northwnd.json"),
      &tests::generic_json_field_factory));
    gens.emplace_back(new tests::json_doc_generator(
      resource("NorthwndEdges.json"),
      &tests::generic_json_field_factory));

    add_segments(*writer, gens);
  }

  auto rdr = open_reader();

  // Name = Product AND source=AdventureWor3ks2014
  {
    irs::And root;
    root.add<irs::by_term>().field("Name").term("Product");
    root.add<irs::by_term>().field("source").term("AdventureWor3ks2014");
    check_query(root, docs_t{}, rdr);
  }
}

TEST_P(boolean_filter_test_case, and_sequential) {
  // add segment
  {
    tests::json_doc_generator gen(
      resource("simple_sequential.json"),
      &tests::generic_json_field_factory);
    add_segment( gen );
  }

  auto rdr = open_reader();

  // empty query
  {
    check_query(irs::And(), docs_t{}, rdr);
  }

  // name=V
  {
    irs::And root;
    root.add<irs::by_term>().field("name").term("V"); // 22

    check_query(root, docs_t{ 22 }, rdr);
  }

  // duplicated=abcd AND same=xyz
  {
    irs::And root;
    root.add<irs::by_term>().field("duplicated").term("abcd"); // 1,5,11,21,27,31
    root.add<irs::by_term>().field("same").term("xyz"); // 1..32
    check_query(root, docs_t{ 1, 5, 11, 21, 27, 31 }, rdr);
  }

  // duplicated=abcd AND same=xyz AND name=A
  {
    irs::And root;
    root.add<irs::by_term>().field("duplicated").term("abcd"); // 1,5,11,21,27,31
    root.add<irs::by_term>().field("same").term("xyz"); // 1..32
    root.add<irs::by_term>().field("name").term("A"); // 1
    check_query(root, docs_t{ 1 }, rdr);
  }

  // duplicated=abcd AND same=xyz AND name=B
  {
    irs::And root;
    root.add<irs::by_term>().field("duplicated").term("abcd"); // 1,5,11,21,27,31
    root.add<irs::by_term>().field("same").term("xyz"); // 1..32
    root.add<irs::by_term>().field("name").term("B"); // 2
    check_query(root, docs_t{}, rdr);
  }
}

TEST_P(boolean_filter_test_case, not_standalone_sequential_ordered) {
  // add segment
  {
    tests::json_doc_generator gen(
      resource("simple_sequential.json"),
      &tests::generic_json_field_factory);
    add_segment(gen);
  }

  auto rdr = open_reader();

  // reverse order
  {
    const std::string column_name = "duplicated";

    std::vector<irs::doc_id_t> expected = { 32, 30, 29, 28, 26, 25, 24, 23, 22, 20, 19, 18, 17, 16, 15, 14, 13, 12, 10, 9, 8, 7, 6, 4, 3, 2 };

    irs::Not not_node;
    not_node.filter<irs::by_term>().field(column_name).term("abcd");

    irs::order order;
    size_t collector_collect_field_count = 0;
    size_t collector_collect_term_count = 0;
    size_t collector_finish_count = 0;
    size_t scorer_score_count = 0;
    auto& sort = order.add<sort::custom_sort>(false);

    sort.collector_collect_field = [&collector_collect_field_count](const irs::sub_reader&, const irs::term_reader&)->void {
      ++collector_collect_field_count;
    };
    sort.collector_collect_term = [&collector_collect_term_count](const irs::sub_reader&, const irs::term_reader&, const irs::attribute_view&)->void {
      ++collector_collect_term_count;
    };
    sort.collectors_collect_ = [&collector_finish_count](irs::byte_type*, const irs::index_reader&, const irs::sort::field_collector*, const irs::sort::term_collector*)->void {
      ++collector_finish_count;
    };
    sort.scorer_add = [](irs::doc_id_t& dst, const irs::doc_id_t& src)->void { ASSERT_TRUE(&dst); ASSERT_TRUE(&src); dst = src; };
    sort.scorer_less = [](const irs::doc_id_t& lhs, const irs::doc_id_t& rhs)->bool { return (lhs > rhs); }; // reverse order
    sort.scorer_score = [&scorer_score_count](irs::doc_id_t& score)->void { ASSERT_TRUE(&score); ++scorer_score_count; };

    auto prepared_order = order.prepare();
    auto prepared_filter = not_node.prepare(*rdr, prepared_order);
    auto score_less = [&prepared_order](
      const irs::bytes_ref& lhs, const irs::bytes_ref& rhs
    )->bool {
      return prepared_order.less(lhs.c_str(), rhs.c_str());
    };
    std::multimap<irs::bstring, irs::doc_id_t, decltype(score_less)> scored_result(score_less);

    ASSERT_EQ(1, rdr->size());
    auto& segment = (*rdr)[0];

    auto filter_itr = prepared_filter->execute(segment, prepared_order);
    ASSERT_EQ(32, irs::cost::extract(filter_itr->attributes()));

    size_t docs_count = 0;
    auto& score = filter_itr->attributes().get<irs::score>();

    // ensure that we avoid COW for pre c++11 std::basic_string
    const irs::bytes_ref score_value = score->value();

    while (filter_itr->next()) {
      score->evaluate();
      ASSERT_FALSE(!score);
      scored_result.emplace(score_value, filter_itr->value());
      ++docs_count;
    }

    ASSERT_EQ(expected.size(), docs_count);

    ASSERT_EQ(0, collector_collect_field_count); // should not be executed (a negated possibly complex filter)
    ASSERT_EQ(0, collector_collect_term_count); // should not be executed
    ASSERT_EQ(1, collector_finish_count); // from "all" query
    ASSERT_EQ(expected.size(), scorer_score_count);

    std::vector<irs::doc_id_t> actual;

    for (auto& entry: scored_result) {
      actual.emplace_back(entry.second);
    }

    ASSERT_EQ(expected, actual);
  }
}

TEST_P(boolean_filter_test_case, not_sequential_ordered) {
  // add segment
  {
    tests::json_doc_generator gen(
      resource("simple_sequential.json"),
      &tests::generic_json_field_factory);
    add_segment(gen);
  }

  auto rdr = open_reader();

  // reverse order
  {
    const std::string column_name = "duplicated";

    std::vector<irs::doc_id_t> expected = { 32, 30, 29, 28, 26, 25, 24, 23, 22, 20, 19, 18, 17, 16, 15, 14, 13, 12, 10, 9, 8, 7, 6, 4, 3, 2 };

    irs::And root;
    root.add<irs::Not>().filter<irs::by_term>().field(column_name).term("abcd");

    irs::order order;
    size_t collector_collect_field_count = 0;
    size_t collector_collect_term_count = 0;
    size_t collector_finish_count = 0;
    size_t scorer_score_count = 0;
    auto& sort = order.add<sort::custom_sort>(false);

    sort.collector_collect_field = [&collector_collect_field_count](const irs::sub_reader&, const irs::term_reader&)->void {
      ++collector_collect_field_count;
    };
    sort.collector_collect_term = [&collector_collect_term_count](const irs::sub_reader&, const irs::term_reader&, const irs::attribute_view&)->void {
      ++collector_collect_term_count;
    };
    sort.collectors_collect_ = [&collector_finish_count](irs::byte_type*, const irs::index_reader&, const irs::sort::field_collector*, const irs::sort::term_collector*)->void {
      ++collector_finish_count;
    };
    sort.scorer_add = [](irs::doc_id_t& dst, const irs::doc_id_t& src)->void { ASSERT_TRUE(&dst); ASSERT_TRUE(&src); dst = src; };
    sort.scorer_less = [](const irs::doc_id_t& lhs, const irs::doc_id_t& rhs)->bool { return (lhs > rhs); }; // reverse order
    sort.scorer_score = [&scorer_score_count](irs::doc_id_t& score)->void { ASSERT_TRUE(&score); ++scorer_score_count; };

    auto prepared_order = order.prepare();
    auto prepared_filter = root.prepare(*rdr, prepared_order);
    auto score_less = [&prepared_order](
      const irs::bytes_ref& lhs, const irs::bytes_ref& rhs
    )->bool {
      return prepared_order.less(lhs.c_str(), rhs.c_str());
    };
    std::multimap<irs::bstring, irs::doc_id_t, decltype(score_less)> scored_result(score_less);

    ASSERT_EQ(1, rdr->size());
    auto& segment = (*rdr)[0];

    auto filter_itr = prepared_filter->execute(segment, prepared_order);
    ASSERT_EQ(32, irs::cost::extract(filter_itr->attributes()));

    size_t docs_count = 0;
    auto& score = filter_itr->attributes().get<irs::score>();

    // ensure that we avoid COW for pre c++11 std::basic_string
    const irs::bytes_ref score_value = score->value();

    while (filter_itr->next()) {
      score->evaluate();
      ASSERT_FALSE(!score);
      scored_result.emplace(score_value, filter_itr->value());
      ++docs_count;
    }

    ASSERT_EQ(expected.size(), docs_count);

    ASSERT_EQ(0, collector_collect_field_count); // should not be executed (a negated possibly complex filter)
    ASSERT_EQ(0, collector_collect_term_count); // should not be executed
    ASSERT_EQ(1, collector_finish_count); // from "all" query
    ASSERT_EQ(expected.size(), scorer_score_count);

    std::vector<irs::doc_id_t> actual;

    for (auto& entry: scored_result) {
      actual.emplace_back(entry.second);
    }

    ASSERT_EQ(expected, actual);
  }
}


TEST_P(boolean_filter_test_case, not_sequential) {
  // add segment
  {
    tests::json_doc_generator gen(
      resource("simple_sequential.json"),
      &tests::generic_json_field_factory);
    add_segment( gen );
  }

  auto rdr = open_reader();

  // empty query
  {
    check_query(irs::Not(), docs_t{}, rdr);
  }

  // single not statement - empty result
  {
    irs::Not not_node;
    not_node.filter<irs::by_term>().field("same").term("xyz"),

    check_query(not_node, docs_t{}, rdr);
  }

  // duplicated=abcd AND (NOT ( NOT name=A ))
  {
    irs::And root;
    root.add<irs::by_term>().field("duplicated").term("abcd");
    root.add<irs::Not>().filter<irs::Not>().filter<irs::by_term>().field("name").term("A");
    check_query(root, docs_t{ 1 }, rdr);
  }

  // duplicated=abcd AND (NOT ( NOT (NOT (NOT ( NOT name=A )))))
  {
    irs::And root;
    root.add<irs::by_term>().field("duplicated").term("abcd");
    root.add<irs::Not>().filter<irs::Not>().filter<irs::Not>().filter<irs::Not>().filter<irs::Not>().filter<irs::by_term>().field("name").term("A");
    check_query(root, docs_t{ 5, 11, 21, 27, 31 }, rdr);
  }

  // * AND NOT *
  {
    {
      irs::And root;
      root.add<irs::all>();
      root.add<irs::Not>().filter<irs::all>();
      check_query(root, docs_t{ }, rdr);
    }

    {
      irs::Or root;
      root.add<irs::all>();
      root.add<irs::Not>().filter<irs::all>();
      check_query(root, docs_t{ }, rdr);
    }
  }

  // duplicated=abcd AND NOT name=A
  {
    {
      irs::And root;
      root.add<irs::by_term>().field("duplicated").term("abcd");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("A");
      check_query(root, docs_t{ 5, 11, 21, 27, 31 }, rdr);
    }

    {
      irs::Or root;
      root.add<irs::by_term>().field("duplicated").term("abcd");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("A");
      check_query(root, docs_t{ 5, 11, 21, 27, 31 }, rdr);
    }
  }

  // duplicated=abcd AND NOT name=A AND NOT name=A
  {
    {
      irs::And root;
      root.add<irs::by_term>().field("duplicated").term("abcd");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("A");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("A");
      check_query(root, docs_t{ 5, 11, 21, 27, 31 }, rdr);
    }

    {
      irs::Or root;
      root.add<irs::by_term>().field("duplicated").term("abcd");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("A");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("A");
      check_query(root, docs_t{ 5, 11, 21, 27, 31 }, rdr);
    }
  }

  // duplicated=abcd AND NOT name=A AND NOT name=E
  {
    {
      irs::And root;
      root.add<irs::by_term>().field("duplicated").term("abcd");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("A");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("E");
      check_query(root, docs_t{ 11, 21, 27, 31 }, rdr);
    }

    {
      irs::Or root;
      root.add<irs::by_term>().field("duplicated").term("abcd");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("A");
      root.add<irs::Not>().filter<irs::by_term>().field("name").term("E");
      check_query(root, docs_t{ 11, 21, 27, 31 }, rdr);
    }
  }
}


TEST_P(boolean_filter_test_case, not_standalone_sequential) {
  // add segment
  {
    tests::json_doc_generator gen(
      resource("simple_sequential.json"),
      &tests::generic_json_field_factory);
    add_segment( gen );
  }

  auto rdr = open_reader();

  // empty query
  {
    check_query(irs::Not(), docs_t{}, rdr);
  }

  // single not statement - empty result
  {
    irs::Not not_node;
    not_node.filter<irs::by_term>().field("same").term("xyz"),

    check_query(not_node, docs_t{}, rdr);
  }

  // single not statement - all docs
  {
    irs::Not not_node;
    not_node.filter<irs::by_term>().field("same").term("invalid_term"),

    check_query(not_node, docs_t{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 }, rdr);
  }

  // (NOT (NOT name=A))
  {
    irs::Not not_node;
    not_node.filter<irs::Not>().filter<irs::by_term>().field("name").term("A");
    check_query(not_node, docs_t{ 1 }, rdr);
  }

  // (NOT (NOT (NOT (NOT (NOT name=A)))))
  {
    irs::Not not_node;
    not_node.filter<irs::Not>().filter<irs::Not>().filter<irs::Not>().filter<irs::Not>().filter<irs::by_term>().field("name").term("A");

    check_query(not_node, docs_t{ 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 }, rdr);
  }
}

TEST_P(boolean_filter_test_case, mixed) {
  {
    // add segment
    {
      tests::json_doc_generator gen(
        resource("simple_sequential.json"),
        &tests::generic_json_field_factory);
      add_segment( gen );
    }

    auto rdr = open_reader();

    // (same=xyz AND duplicated=abcd) OR (same=xyz AND duplicated=vczc)
    {
      irs::Or root;

      // same=xyz AND duplicated=abcd
      {
        irs::And& child = root.add<irs::And>();
        child.add<irs::by_term>().field("same").term("xyz");
        child.add<irs::by_term>().field("duplicated").term("abcd");
      }

      // same=xyz AND duplicated=vczc
      {
        irs::And& child = root.add<irs::And>();
        child.add<irs::by_term>().field("same").term("xyz");
        child.add<irs::by_term>().field("duplicated").term("vczc");
      }

      check_query(root, docs_t{ 1, 2, 3, 5, 8, 11, 14, 17, 19, 21, 24, 27, 31 }, rdr);
    }

    // ((same=xyz AND duplicated=abcd) OR (same=xyz AND duplicated=vczc)) AND name=X
    {
      irs::And root;
      root.add<irs::by_term>().field("name").term("X");

      // ( same = xyz AND duplicated = abcd ) OR( same = xyz AND duplicated = vczc )
      {
        irs::Or& child = root.add<irs::Or>();

        // same=xyz AND duplicated=abcd
        {
          irs::And& subchild = child.add<irs::And>();
          subchild.add<irs::by_term>().field("same").term("xyz");
          subchild.add<irs::by_term>().field("duplicated").term("abcd");
        }

        // same=xyz AND duplicated=vczc
        {
          irs::And& subchild = child.add<irs::And>();
          subchild.add<irs::by_term>().field("same").term("xyz");
          subchild.add<irs::by_term>().field("duplicated").term("vczc");
        }
      }

      check_query(root, docs_t{ 24 }, rdr);
    }

    // ((same=xyz AND duplicated=abcd) OR (name=A or name=C or NAME=P or name=U or name=X)) OR (same=xyz AND (duplicated=vczc OR (name=A OR name=C OR NAME=P OR name=U OR name=X)) )
    // 1, 2, 3, 4, 5, 8, 11, 14, 16, 17, 19, 21, 24, 27, 31
    {
      irs::Or root;

      // (same=xyz AND duplicated=abcd) OR (name=A or name=C or NAME=P or name=U or name=X)
      // 1, 3, 5,11, 16, 21, 24, 27, 31
      {
        irs::Or& child = root.add<irs::Or>();

        // ( same = xyz AND duplicated = abcd )
        {
          irs::And& subchild = root.add<irs::And>();
          subchild.add<irs::by_term>().field("same").term("xyz");
          subchild.add<irs::by_term>().field("duplicated").term("abcd");
        }

        child.add<irs::by_term>().field("name").term("A");
        child.add<irs::by_term>().field("name").term("C");
        child.add<irs::by_term>().field("name").term("P");
        child.add<irs::by_term>().field("name").term("X");
      }

      // (same=xyz AND (duplicated=vczc OR (name=A OR name=C OR NAME=P OR name=U OR name=X))
      // 1, 2, 3, 8, 14, 16, 17, 19, 21, 24
      {
        irs::And& child = root.add<irs::And>();
        child.add<irs::by_term>().field("same").term("xyz");

        // (duplicated=vczc OR (name=A OR name=C OR NAME=P OR name=U OR name=X)
        {
          irs::Or& subchild = child.add<irs::Or>();
          subchild.add<irs::by_term>().field("duplicated").term("vczc");

          // name=A OR name=C OR NAME=P OR name=U OR name=X
          {
            irs::Or& subsubchild = subchild.add<irs::Or>();
            subchild.add<irs::by_term>().field("name").term("A");
            subchild.add<irs::by_term>().field("name").term("C");
            subchild.add<irs::by_term>().field("name").term("P");
            subchild.add<irs::by_term>().field("name").term("X");
          }
        }
      }

      check_query(root, docs_t{ 1, 2, 3, 5, 8, 11, 14, 16, 17, 19, 21, 24, 27, 31 }, rdr);
    }

    // (same=xyz AND duplicated=abcd) OR (same=xyz AND duplicated=vczc) AND *
    {
      irs::Or root;

      // *
      root.add<irs::all>();

      // same=xyz AND duplicated=abcd
      {
        irs::And& child = root.add<irs::And>();
        child.add<irs::by_term>().field("same").term("xyz");
        child.add<irs::by_term>().field("duplicated").term("abcd");
      }

      // same=xyz AND duplicated=vczc
      {
        irs::And& child = root.add<irs::And>();
        child.add<irs::by_term>().field("same").term("xyz");
        child.add<irs::by_term>().field("duplicated").term("vczc");
      }

      check_query(
        root,
        docs_t{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 },
        rdr
      );
    }

    // (same=xyz AND duplicated=abcd) OR (same=xyz AND duplicated=vczc) OR NOT *
    {
      irs::Or root;

      // NOT *
      root.add<irs::Not>().filter<irs::all>();

      // same=xyz AND duplicated=abcd
      {
        irs::And& child = root.add<irs::And>();
        child.add<irs::by_term>().field("same").term("xyz");
        child.add<irs::by_term>().field("duplicated").term("abcd");
      }

      // same=xyz AND duplicated=vczc
      {
        irs::And& child = root.add<irs::And>();
        child.add<irs::by_term>().field("same").term("xyz");
        child.add<irs::by_term>().field("duplicated").term("vczc");
      }

      check_query(root, docs_t{}, rdr);
    }
  }
}

#ifndef IRESEARCH_DLL

TEST_P(boolean_filter_test_case, mixed_ordered) {
  // add segment
  {
    tests::json_doc_generator gen(
      resource("simple_sequential.json"),
      &tests::generic_json_field_factory);
    add_segment( gen );
  }

  auto rdr = open_reader();
  ASSERT_TRUE(bool(rdr));

  {
    irs::Or root;
    auto& sub = root.add<irs::And>();
    sub.add<irs::by_range>().field("name").include<irs::Bound::MIN>(false).term<irs::Bound::MIN>("!");
    sub.add<irs::by_range>().field("name").include<irs::Bound::MAX>(false).term<irs::Bound::MAX>("~");

    irs::order ord;
    ord.add<irs::tfidf_sort>(false);
    ord.add<irs::bm25_sort>(false);

    auto prepared_ord = ord.prepare();
    ASSERT_FALSE(prepared_ord.empty());
    ASSERT_EQ(2, prepared_ord.size());

    auto prepared = root.prepare(*rdr, prepared_ord);
    ASSERT_NE(nullptr, prepared);

    std::vector<irs::doc_id_t> expected_docs {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
      15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
      29, 30, 31, 32
    };

    auto expected_doc = expected_docs.begin();
    for (const auto& sub: rdr) {
      auto docs = prepared->execute(sub, prepared_ord);

      auto& doc = docs->attributes().get<irs::document>();
      ASSERT_TRUE(bool(doc)); // ensure all iterators contain "document" attribute

      const auto* score = docs->attributes().get<irs::score>().get();
      ASSERT_NE(nullptr, score);

      // ensure that we avoid COW for pre c++11 std::basic_string
      irs::bytes_ref score_value = score->value();

      std::vector<irs::bstring> scores;
      while (docs->next()) {
        EXPECT_EQ(*expected_doc, doc->value);
        ++expected_doc;

        score->evaluate();
        scores.emplace_back(score_value.c_str(), score_value.size());
      }

      ASSERT_EQ(expected_docs.end(), expected_doc);
      ASSERT_TRUE(irs::irstd::all_equal(scores.begin(), scores.end()));
    }
  }
}

#endif

// ----------------------------------------------------------------------------
// --SECTION--                                                   Not base tests
// ----------------------------------------------------------------------------

TEST(Not_test, ctor) {
  irs::Not q;
  ASSERT_EQ(irs::Not::type(), q.type());
  ASSERT_EQ(nullptr, q.filter());
  ASSERT_EQ(irs::no_boost(), q.boost());
}

TEST(Not_test, equal) {
  {
    irs::Not lhs, rhs;
    ASSERT_EQ(lhs, rhs);
    ASSERT_EQ(lhs.hash(), rhs.hash());
  }

  {
    irs::Not lhs;
    lhs.filter<irs::by_term>().field("abc").term("def");

    irs::Not rhs;
    rhs.filter<irs::by_term>().field("abc").term("def");
    ASSERT_EQ(lhs, rhs);
    ASSERT_EQ(lhs.hash(), rhs.hash());
  }

  {
    irs::Not lhs;
    lhs.filter<irs::by_term>().field("abc").term("def");

    irs::Not rhs;
    rhs.filter<irs::by_term>().field("abcd").term("def");
    ASSERT_NE(lhs, rhs);
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                                                   And base tests 
// ----------------------------------------------------------------------------

TEST(And_test, ctor) {
  irs::And q;
  ASSERT_EQ(irs::And::type(), q.type());
  ASSERT_TRUE(q.empty());
  ASSERT_EQ(0, q.size());
  ASSERT_EQ(irs::no_boost(), q.boost());
}

TEST(And_test, add_clear) {
  irs::And q;
  q.add<irs::by_term>();
  q.add<irs::by_term>();
  ASSERT_FALSE(q.empty());
  ASSERT_EQ(2, q.size());
  q.clear();
  ASSERT_TRUE(q.empty());
  ASSERT_EQ(0, q.size());
}

TEST(And_test, equal) {
  irs::And lhs;
  lhs.add<irs::by_term>().field("field").term("term");
  lhs.add<irs::by_term>().field("field1").term("term1");
  {
    irs::And& subq = lhs.add<irs::And>();
    subq.add<irs::by_term>().field("field123").term("dfterm");
    subq.add<irs::by_term>().field("fieasfdld1").term("term1");
  }

  {
    irs::And rhs;
    rhs.add<irs::by_term>().field("field").term("term");
    rhs.add<irs::by_term>().field("field1").term("term1");
    {
      irs::And& subq = rhs.add<irs::And>();
      subq.add<irs::by_term>().field("field123").term("dfterm");
      subq.add<irs::by_term>().field("fieasfdld1").term("term1");
    }

    ASSERT_EQ(lhs, rhs);
    ASSERT_EQ(lhs.hash(), rhs.hash());
  }

  {
    irs::And rhs;
    rhs.add<irs::by_term>().field("field").term("term");
    rhs.add<irs::by_term>().field("field1").term("term1");
    {
      irs::And& subq = rhs.add<irs::And>();
      subq.add<irs::by_term>().field("field123").term("dfterm");
      subq.add<irs::by_term>().field("fieasfdld1").term("term1");
      subq.add<irs::by_term>().field("fieasfdld1").term("term1");
    }

    ASSERT_NE(lhs, rhs);
  }
}

#ifndef IRESEARCH_DLL

TEST(And_test, optimize_double_negation) {
  irs::And root;
  auto& term = root.add<irs::Not>().filter<irs::Not>().filter<irs::by_term>();
  term.field("test_field").term("test_term");

  auto prepared = root.prepare(irs::sub_reader::empty());
  ASSERT_NE(nullptr, dynamic_cast<const irs::term_query*>(prepared.get()));
}

TEST(And_test, prepare_empty_filter) {
  irs::And root;
  auto prepared = root.prepare(irs::sub_reader::empty());
  ASSERT_NE(nullptr, prepared);
  ASSERT_EQ(typeid(irs::filter::prepared::empty().get()), typeid(prepared.get()));
}

TEST(And_test, optimize_single_node) {
  // simple hierarchy
  {
    irs::And root;
    auto& term = root.add<irs::by_term>();
    term.field("test_field").term("test_term");

    auto prepared = root.prepare(irs::sub_reader::empty());
    ASSERT_NE(nullptr, dynamic_cast<const irs::term_query*>(prepared.get()));
  }

  // complex hierarchy
  {
    irs::And root;
    auto& term = root.add<irs::And>().add<irs::And>().add<irs::by_term>();
    term.field("test_field").term("test_term");

    auto prepared = root.prepare(irs::sub_reader::empty());
    ASSERT_NE(nullptr, dynamic_cast<const irs::term_query*>(prepared.get()));
  }
}

TEST(And_test, optimize_all_filters) {
  // single `all` filter
  {
    irs::And root;
    root.add<irs::all>().boost(5.f);

    auto prepared = root.prepare(irs::sub_reader::empty());
    ASSERT_EQ(typeid(irs::all().prepare(irs::sub_reader::empty()).get()), typeid(prepared.get()));
    ASSERT_EQ(5.f, prepared->boost());
  }

  // multiple `all` filters
  {
    irs::And root;
    root.add<irs::all>().boost(5.f);
    root.add<irs::all>().boost(2.f);
    root.add<irs::all>().boost(3.f);

    auto prepared = root.prepare(irs::sub_reader::empty());
    ASSERT_EQ(typeid(irs::all().prepare(irs::sub_reader::empty()).get()), typeid(prepared.get()));
    ASSERT_EQ(30.f, prepared->boost());
  }

  // multiple `all` filters + term filter
  {
    irs::And root;
    auto& term = root.add<irs::by_term>();
    term.field("test_field").term("test_term");
    root.add<irs::all>().boost(5.f);
    root.add<irs::all>().boost(2.f);

    auto prepared = root.prepare(irs::sub_reader::empty());
    ASSERT_NE(nullptr, dynamic_cast<const irs::term_query*>(prepared.get()));
    ASSERT_EQ(10.f, prepared->boost());
  }

  // `all` filter + term filter
  {
    irs::And root;
    auto& term = root.add<irs::by_term>();
    term.field("test_field").term("test_term");
    root.add<irs::all>().boost(5.f);

    auto prepared = root.prepare(irs::sub_reader::empty());
    ASSERT_NE(nullptr, dynamic_cast<const irs::term_query*>(prepared.get()));
    ASSERT_EQ(5.f, prepared->boost());
  }
}

#endif // IRESEARCH_DLL

// ----------------------------------------------------------------------------
// --SECTION--                                                    Or base tests 
// ----------------------------------------------------------------------------

TEST(Or_test, ctor) {
  irs::Or q;
  ASSERT_EQ(irs::Or::type(), q.type());
  ASSERT_TRUE(q.empty());
  ASSERT_EQ(0, q.size());
  ASSERT_EQ(1, q.min_match_count());
  ASSERT_EQ(irs::no_boost(), q.boost());
}

TEST(Or_test, add_clear) {
  irs::Or q;
  q.add<irs::by_term>();
  q.add<irs::by_term>();
  ASSERT_FALSE(q.empty());
  ASSERT_EQ(2, q.size());
  q.clear();
  ASSERT_TRUE(q.empty());
  ASSERT_EQ(0, q.size());
}

TEST(Or_test, equal) {
  irs::Or lhs;
  lhs.add<irs::by_term>().field("field").term("term");
  lhs.add<irs::by_term>().field("field1").term("term1");
  {
    irs::And& subq = lhs.add<irs::And>();
    subq.add<irs::by_term>().field("field123").term("dfterm");
    subq.add<irs::by_term>().field("fieasfdld1").term("term1");
  }

  {
    irs::Or rhs;
    rhs.add<irs::by_term>().field("field").term("term");
    rhs.add<irs::by_term>().field("field1").term("term1");
    {
      irs::And& subq = rhs.add<irs::And>();
      subq.add<irs::by_term>().field("field123").term("dfterm");
      subq.add<irs::by_term>().field("fieasfdld1").term("term1");
    }

    ASSERT_EQ(lhs, rhs);
    ASSERT_EQ(lhs.hash(), rhs.hash());
  }

  {
    irs::Or rhs;
    rhs.add<irs::by_term>().field("field").term("term");
    rhs.add<irs::by_term>().field("field1").term("term1");
    {
      irs::And& subq = rhs.add<irs::And>();
      subq.add<irs::by_term>().field("field123").term("dfterm");
      subq.add<irs::by_term>().field("fieasfdld1").term("term1");
      subq.add<irs::by_term>().field("fieasfdld1").term("term1");
    }

    ASSERT_NE(lhs, rhs);
  }
}

#ifndef IRESEARCH_DLL

TEST(Or_test, optimize_double_negation) {
  irs::Or root;
  auto& term = root.add<irs::Not>().filter<irs::Not>().filter<irs::by_term>();
  term.field("test_field").term("test_term");

  auto prepared = root.prepare(irs::sub_reader::empty());
  ASSERT_NE(nullptr, dynamic_cast<const irs::term_query*>(prepared.get()));
}

TEST(Or_test, optimize_single_node) {
  // simple hierarchy
  {
    irs::Or root;
    auto& term = root.add<irs::by_term>();
    term.field("test_field").term("test_term");

    auto prepared = root.prepare(irs::sub_reader::empty());
    ASSERT_NE(nullptr, dynamic_cast<const irs::term_query*>(prepared.get()));
  }

  // complex hierarchy
  {
    irs::Or root;
    auto& term = root.add<irs::Or>().add<irs::Or>().add<irs::by_term>();
    term.field("test_field").term("test_term");

    auto prepared = root.prepare(irs::sub_reader::empty());
    ASSERT_NE(nullptr, dynamic_cast<const irs::term_query*>(prepared.get()));
  }
}

#endif // IRESEARCH_DLL

INSTANTIATE_TEST_CASE_P(
  boolean_filter_test,
  boolean_filter_test_case,
  ::testing::Combine(
    ::testing::Values(
      &tests::memory_directory,
      &tests::fs_directory,
      &tests::mmap_directory
    ),
    ::testing::Values("1_0")
  ),
  tests::to_string
);

NS_END // tests

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
