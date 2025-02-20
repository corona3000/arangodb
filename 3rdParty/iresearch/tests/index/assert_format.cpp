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
#include "assert_format.hpp"

#include "analysis/token_attributes.hpp"
#include "analysis/token_stream.hpp"

#include "index/field_meta.hpp"
#include "index/directory_reader.hpp"

#include "search/term_filter.hpp"
#include "search/boolean_filter.hpp"
#include "search/tfidf.hpp"
#include "search/cost.hpp"
#include "search/score.hpp"

#include "utils/bit_utils.hpp"
#include "utils/automaton_utils.hpp"
#include "utils/fst_table_matcher.hpp"

#include "store/data_output.hpp"

#include <unordered_set>
#include <algorithm>
#include <iostream>
#include <cassert>

NS_BEGIN(tests)

position::position(
    uint32_t pos, uint32_t start,
    uint32_t end,
    const irs::bytes_ref& pay)
  : pos(pos), start(start),
    end(end), payload(pay) {
}

posting::posting(irs::doc_id_t id)
  : id_(id) {
}

void posting::add(uint32_t pos, uint32_t offs_start, const irs::attribute_view& attrs) {
  auto& offs = attrs.get<irs::offset>();
  auto& pay = attrs.get<irs::payload>();

  uint32_t start = irs::offset::INVALID_OFFSET;
  uint32_t end = irs::offset::INVALID_OFFSET;
  if (offs) {
    start = offs_start + offs->start;
    end = offs_start + offs->end;
  }

  positions_.emplace(pos, start, end, pay ? pay->value : irs::bytes_ref::NIL);
}

term::term(const irs::bytes_ref& data): value( data ) {}

posting& term::add(irs::doc_id_t id) {
  return const_cast<posting&>(*postings.emplace(id).first);
}

bool term::operator<( const term& rhs ) const {
  return irs::memcmp_less(
    value.c_str(), value.size(),
    rhs.value.c_str(), rhs.value.size()
  );
}

field::field(
    const irs::string_ref& name,
    const irs::flags& features)
  : pos(0), offs(0) {
  this->name = name;
  this->features = features;
}

field::field(field&& rhs) noexcept
  : field_meta(std::move(rhs)),
    terms( std::move(rhs.terms)),
    docs(std::move(rhs.docs)),
    pos( rhs.pos ),
    offs( rhs.offs ) {
}

term& field::add(const irs::bytes_ref& t) {
  auto res = terms.emplace( t );
  return const_cast< term& >( *res.first );
}

term* field::find(const irs::bytes_ref& t) {
  auto it = terms.find( term( t ) );
  return terms.end() == it ? nullptr : const_cast< term* >(&*it);
}

size_t field::remove(const irs::bytes_ref& t) {
  return terms.erase( term( t ) );
}

/* -------------------------------------------------------------------
* index_segment
* ------------------------------------------------------------------*/

index_segment::index_segment() : count_( 0 ) {}

index_segment::index_segment(index_segment&& rhs) noexcept
  : fields_( std::move( rhs.fields_)),
    count_( rhs.count_) {
  rhs.count_ = 0;
}

index_segment& index_segment::operator=(index_segment&& rhs) noexcept {
  if ( this != &rhs ) {
    fields_ = std::move( rhs.fields_ );
    count_ = rhs.count_;
    rhs.count_ = 0;
  }

  return *this;
}

void index_segment::add_sorted(const ifield& f) {
  irs::bstring buf;
  irs::bytes_output out(buf);
  if (f.write(out)) {
    const irs::bytes_ref value = buf;
    const auto doc_id = irs::doc_id_t((irs::doc_limits::min)() + count_);
    sort_.emplace_back(std::make_pair(irs::bstring(value.c_str(), value.size()), doc_id));
  }
}

void index_segment::add(const ifield& f) {
  const irs::string_ref& field_name = f.name();
  field field(field_name, f.features());
  auto res = fields_.emplace(field_name, std::move(field));
  if (res.second) {
    id_to_field_.emplace_back(&res.first->second);
  }
  const_cast<irs::string_ref&>(res.first->first) = res.first->second.name;
  auto& fld = res.first->second;

  auto& stream = f.get_tokens();

  auto& attrs = stream.attributes();
  auto& term = attrs.get<irs::term_attribute>();
  auto& inc = attrs.get<irs::increment>();
  auto& offs = attrs.get<irs::offset>();

  bool empty = true;
  auto doc_id = irs::doc_id_t((irs::doc_limits::min)() + count_);

  while (stream.next()) {
    tests::term& trm = fld.add(term->value());
    tests::posting& pst = trm.add(doc_id);
    fld.pos += inc->value;
    pst.add(fld.pos, fld.offs, attrs);
    empty = false;
  }

  if (!empty) { 
    fld.docs.emplace(doc_id);
  }

  if (offs) {
    fld.offs += offs->end;
  }
}

std::string index_meta_writer::filename(
  const irs::index_meta& meta
) const {
  return std::string();
}

bool index_meta_writer::prepare(
  irs::directory& dir,
  irs::index_meta& meta
) {
  return true;
}

bool index_meta_writer::commit() { return true; }

void index_meta_writer::rollback() noexcept { }

/* -------------------------------------------------------------------
 * index_meta_reader
 * ------------------------------------------------------------------*/

bool index_meta_reader::last_segments_file(
    const irs::directory&,
    std::string&) const {
  return false;
}

void index_meta_reader::read(
  const irs::directory& dir,
  irs::index_meta& meta,
  const irs::string_ref& filename /*= string_ref::NIL*/
) {
}

/* -------------------------------------------------------------------
 * segment_meta_writer 
 * ------------------------------------------------------------------*/


void segment_meta_writer::write(
  irs::directory& dir,
  std::string& filename,
  const irs::segment_meta& meta ) {
}

/* -------------------------------------------------------------------
 * segment_meta_reader
 * ------------------------------------------------------------------*/

void segment_meta_reader::read( 
  const irs::directory& dir,
  irs::segment_meta& meta,
  const irs::string_ref& filename /*= string_ref::NIL*/
) {
}

/* -------------------------------------------------------------------
* document_mask_writer
* ------------------------------------------------------------------*/

document_mask_writer::document_mask_writer(const index_segment& data):
  data_(data) {
}

std::string document_mask_writer::filename(
    const irs::segment_meta& meta
) const {
  return std::string();
}

void document_mask_writer::write(
    irs::directory& dir,
    const irs::segment_meta& meta,
    const irs::document_mask& docs_mask
) {
  EXPECT_EQ(data_.doc_mask().size(), docs_mask.size());
  for (auto doc_id : docs_mask) {
    EXPECT_EQ(true, data_.doc_mask().find(doc_id) != data_.doc_mask().end());
  }
}

/* -------------------------------------------------------------------
 * field_writer
 * ------------------------------------------------------------------*/

field_writer::field_writer(const index_segment& data, const irs::flags& features)
  : readers_( data ), features_( features) {
}

void field_writer::prepare(const irs::flush_state& state) {
  EXPECT_EQ( state.doc_count, readers_.data().doc_count() );
}

void field_writer::write(
    const std::string& name,
    irs::field_id norm,
    const irs::flags& expected_field,
    irs::term_iterator& actual_term) {
  // features to check
  const tests::field* fld = readers_.data().find(name);
  ASSERT_NE(nullptr, fld);
  ASSERT_EQ(fld->name, name);
  ASSERT_EQ(fld->norm, norm);
  ASSERT_EQ(fld->features, expected_field);
  
  auto features = features_ & fld->features;

  const irs::term_reader* expected_term_reader = readers_.field(fld->name);
  ASSERT_NE(nullptr, expected_term_reader);

  irs::bytes_ref actual_min{ irs::bytes_ref::NIL };
  irs::bytes_ref actual_max{ irs::bytes_ref::NIL };
  irs::bstring actual_min_buf;
  irs::bstring actual_max_buf;
  size_t actual_size = 0;

  for (auto expected_term = expected_term_reader->iterator(); 
       actual_term.next(); ++actual_size) {
    ASSERT_TRUE(expected_term->next());

    assert_term(*expected_term, actual_term, features);

    if (actual_min.null()) {
      actual_min_buf = actual_term.value();
      actual_min = actual_min_buf;
    }

    actual_max_buf = actual_term.value();
    actual_max = actual_max_buf;
  }

  // check term reader
  ASSERT_EQ(expected_term_reader->size(), actual_size);
  ASSERT_EQ((expected_term_reader->min)(), actual_min);
  ASSERT_EQ((expected_term_reader->max)(), actual_max);
}

void field_writer::end() { }

/* -------------------------------------------------------------------
 * field_reader
 * ------------------------------------------------------------------*/

class pos_iterator;

class doc_iterator : public irs::doc_iterator {
 public:
   doc_iterator(const irs::flags& features,
                      const tests::term& data );

  irs::doc_id_t value() const override {
    return doc_.value;
  }

  const irs::attribute_view& attributes() const noexcept override {
    return attrs_;
  }

  virtual bool next() override {
    if (next_ == data_.postings.end()) {
      doc_.value = irs::type_limits<irs::type_t::doc_id_t>::eof();
      return false;
    }

    prev_ = next_, ++next_;
    doc_.value = prev_->id();
    freq_.value = prev_->positions().size();
    pos_.clear();

    return true;
  }

  virtual irs::doc_id_t seek(irs::doc_id_t id) override {
    auto it = data_.postings.find(id);

    if ( it == data_.postings.end() ) {
      prev_ = next_ = it;
      return irs::type_limits<irs::type_t::doc_id_t>::eof();
    }

    prev_ = it;
    next_ = ++it;
    doc_.value = prev_->id();
    pos_.clear();

    return doc_.value;
  }

 private:
  class pos_iterator: public irs::position {
   public:
    pos_iterator(const doc_iterator& owner, const irs::flags& features)
      : irs::position(2), // offset + payload
        owner_(owner) {
      if (features.check<irs::offset>()) {
        attrs_.emplace(offs_);
      }

      if (features.check<irs::payload>()) {
        attrs_.emplace(pay_);
      }
    }

    void clear() {
      next_ = owner_.prev_->positions().begin();
      value_ = irs::type_limits<irs::type_t::pos_t>::invalid();
      offs_.clear();
      pay_.clear();
    }

    bool next() override {
      if ( next_ == owner_.prev_->positions().end() ) {
        value_ = irs::type_limits<irs::type_t::pos_t>::eof();
        return false;
      }

      value_ = next_->pos;
      offs_.start = next_->start;
      offs_.end = next_->end;
      pay_.value = next_->payload;
      ++next_;

      return true;
    }

    virtual void reset() override {
      assert(false); // unsupported
    }

   private:
    std::set<tests::position>::const_iterator next_;
    irs::offset offs_;
    irs::payload pay_;
    const doc_iterator& owner_;
  };

  irs::attribute_view attrs_;
  irs::document doc_;
  irs::frequency freq_;
  irs::cost cost_;
  irs::score score_;
  pos_iterator pos_;
  const irs::flags& features_;
  const tests::term& data_;
  std::set<posting>::const_iterator prev_;
  std::set<posting>::const_iterator next_;
};

doc_iterator::doc_iterator(const irs::flags& features, const tests::term& data)
  : features_( features ),
    data_(data),
    pos_(*this, features) {
  next_ = data_.postings.begin();

  cost_.value(data_.postings.size());
  attrs_.emplace(cost_);

  attrs_.emplace(doc_);
  attrs_.emplace(score_);

  attrs_.emplace(cost_);

  if (features.check<irs::frequency>()) {
    attrs_.emplace(freq_);
  }

  if (features.check<irs::position>()) {
    attrs_.emplace(pos_);
  }
}

class term_iterator : public irs::seek_term_iterator {
 public:
  term_iterator( const tests::field& data ) 
    : data_( data ) {
    next_ = data_.terms.begin();
  }

  const irs::attribute_view& attributes() const noexcept override {
    return attrs_;
  }

  const irs::bytes_ref& value() const override {
    return value_;
  }

  bool next() override {
    if ( next_ == data_.terms.end() ) {
      value_ = irs::bytes_ref::NIL;
      return false;
    }

    prev_ = next_, ++next_;
    value_ = prev_->value;
    return true;
  }

  virtual void read() override  { }

  virtual bool seek(const irs::bytes_ref& value) override {
    auto it = data_.terms.find(value);

    if (it == data_.terms.end()) {
      prev_ = next_ = it;
      value_ = irs::bytes_ref::NIL;
      return false;
    }

    prev_ = it;
    next_ = ++it;
    value_ = prev_->value;
    return true;
  }

  virtual irs::SeekResult seek_ge(const irs::bytes_ref& value) override {
    auto it = data_.terms.lower_bound(value);
    if (it == data_.terms.end()) {
      prev_ = next_ = it;
      value_ = irs::bytes_ref::NIL;
      return irs::SeekResult::END;
    }

    if (it->value == value) {
      prev_ = it;
      next_ = ++it;
    value_ = prev_->value;
      return irs::SeekResult::FOUND;
    }

    prev_ = ++it;
    next_ = ++it;
    value_ = prev_->value;
    return irs::SeekResult::NOT_FOUND;
  }

  virtual doc_iterator::ptr postings(const irs::flags& features) const override {
    return doc_iterator::make<doc_iterator>(data_.features & features, *prev_);
  }

  virtual bool seek(
      const irs::bytes_ref& term,
      const irs::seek_term_iterator::seek_cookie& cookie
  ) override {
    return false;
  }

  virtual irs::seek_term_iterator::seek_cookie::ptr cookie() const override {
    return nullptr;
  }

 private:
  irs::attribute_view attrs_;
  const tests::field& data_;
  std::set< tests::term >::const_iterator prev_;
  std::set< tests::term >::const_iterator next_;
  irs::bytes_ref value_;
};

irs::seek_term_iterator::ptr term_reader::iterator() const {
  return irs::memory::make_managed<irs::seek_term_iterator>(
    irs::memory::make_unique<term_iterator>(data_)
  );
}

irs::seek_term_iterator::ptr term_reader::iterator(irs::automaton_table_matcher& matcher) const {
  return irs::memory::make_managed<irs::seek_term_iterator>(
    irs::memory::make_unique<irs::automaton_term_iterator>(matcher.GetFst(), iterator())
  );
}

field_reader::field_reader(const index_segment& data)
  : data_(data) {

  readers_.reserve(data.fields().size());

  for (const auto& pair : data_.fields()) {
    readers_.emplace_back(irs::term_reader::make<term_reader>(pair.second));
  }
}

field_reader::field_reader(field_reader&& other) noexcept
  : readers_(std::move(other.readers_)), data_(std::move(other.data_)) {
}

void field_reader::prepare(
    const irs::directory& dir, const irs::segment_meta& meta, const irs::document_mask& mask
) {
}

irs::field_iterator::ptr field_reader::iterator() const {
  return nullptr;
}

const irs::term_reader* field_reader::field(const irs::string_ref& field) const {
  const auto it = std::lower_bound(
    readers_.begin(), readers_.end(), field,
    [] (const irs::term_reader::ptr& lhs, const irs::string_ref& rhs) {
      return lhs->meta().name < rhs;
  });

  return it == readers_.end() || field < (**it).meta().name
    ? nullptr
    : it->get();
}
  
size_t field_reader::size() const {
  return data_.size();
}

/* -------------------------------------------------------------------
 * format 
 * ------------------------------------------------------------------*/

/*static*/ decltype(format::DEFAULT_SEGMENT) format::DEFAULT_SEGMENT;

format::format(): format(DEFAULT_SEGMENT) {}

format::format(const index_segment& data):
  irs::format(format::type()), data_(data) {
}

irs::index_meta_writer::ptr format::get_index_meta_writer() const {
  return irs::index_meta_writer::make<index_meta_writer>();
}

irs::index_meta_reader::ptr format::get_index_meta_reader() const {
  // can reuse stateless reader
  static index_meta_reader reader;

  return irs::memory::make_managed<irs::index_meta_reader, false>(&reader);
}

irs::segment_meta_writer::ptr format::get_segment_meta_writer() const {
  // can reuse stateless writer
  static segment_meta_writer writer;

  return irs::memory::make_managed<irs::segment_meta_writer, false>(&writer);
}

irs::segment_meta_reader::ptr format::get_segment_meta_reader() const {
  // can reuse stateless reader
  static segment_meta_reader reader;

  return irs::memory::make_managed<irs::segment_meta_reader, false>(&reader);
}

irs::document_mask_reader::ptr format::get_document_mask_reader() const {
  return irs::document_mask_reader::ptr();
}

irs::document_mask_writer::ptr format::get_document_mask_writer() const {
  return irs::document_mask_writer::make<tests::document_mask_writer>(data_);
}

irs::field_writer::ptr format::get_field_writer(bool volatile_attributes) const {
  return irs::field_writer::make<tests::field_writer>(data_);
}

irs::field_reader::ptr format::get_field_reader() const {
  return irs::field_reader::make<tests::field_reader>(data_);
}

irs::column_meta_writer::ptr format::get_column_meta_writer() const {
  return nullptr;
}

irs::column_meta_reader::ptr format::get_column_meta_reader() const {
  return nullptr;
}

irs::columnstore_writer::ptr format::get_columnstore_writer() const {
  return nullptr;
}

irs::columnstore_reader::ptr format::get_columnstore_reader() const {
  return nullptr;
}

DEFINE_FORMAT_TYPE_NAMED(tests::format, "iresearch_format_tests")
REGISTER_FORMAT(tests::format);

/*static*/ irs::format::ptr format::make() {
  static const auto instance = irs::memory::make_shared<format>();
  return instance;
}

void assert_term(
    const irs::term_iterator& expected_term,
    const irs::term_iterator& actual_term,
    const irs::flags& requested_features) {
  ASSERT_EQ(expected_term.value(), actual_term.value());

  const irs::doc_iterator::ptr expected_docs = expected_term.postings(requested_features);
  const irs::doc_iterator::ptr actual_docs = actual_term.postings(requested_features);

  ASSERT_EQ(expected_docs->attributes().features() & requested_features, actual_docs->attributes().features() & requested_features);

  ASSERT_TRUE(!irs::doc_limits::valid(expected_docs->value()));
  ASSERT_TRUE(!irs::doc_limits::valid(actual_docs->value()));
  // check docs
  for (; expected_docs->next();) {
    ASSERT_TRUE(actual_docs->next());
    ASSERT_EQ(expected_docs->value(), actual_docs->value());

    // check document attributes
    {
      auto& expected_attrs = expected_docs->attributes();
      auto& actual_attrs = actual_docs->attributes();

      auto& expected_freq = expected_attrs.get<irs::frequency>();
      auto& actual_freq = actual_attrs.get<irs::frequency>();

      if (expected_freq) {
        ASSERT_FALSE(!actual_freq);
        ASSERT_EQ(expected_freq->value, actual_freq->value);
      }

      auto& expected_pos = expected_attrs.get<irs::position>();
      auto& actual_pos = actual_attrs.get<irs::position>();

      if (expected_pos) {
        ASSERT_FALSE(!actual_pos);

        auto& expected_offs = expected_pos->attributes().get<irs::offset>();
        auto& actual_offs = actual_pos->attributes().get<irs::offset>();
        if (expected_offs) ASSERT_FALSE(!actual_offs);

        auto& expected_pay = expected_pos->attributes().get<irs::payload>();
        auto& actual_pay = actual_pos->attributes().get<irs::payload>();
        if (expected_pay) ASSERT_FALSE(!actual_pay);
        ASSERT_TRUE(!irs::pos_limits::valid(expected_pos->value()));
        ASSERT_TRUE(!irs::pos_limits::valid(actual_pos->value()));
        for (; expected_pos->next();) {
          ASSERT_TRUE(actual_pos->next());
          ASSERT_EQ(expected_pos->value(), actual_pos->value());

          if (expected_offs) {
            ASSERT_EQ(expected_offs->start, actual_offs->start);
            ASSERT_EQ(expected_offs->end, actual_offs->end);
          }

          if (expected_pay) {
            ASSERT_EQ(expected_pay->value, actual_pay->value);
          }
        }
        ASSERT_FALSE(actual_pos->next());
        ASSERT_TRUE(irs::pos_limits::eof(expected_pos->value()));
        ASSERT_TRUE(irs::pos_limits::eof(actual_pos->value()));
      }
    }
  }
  ASSERT_FALSE(actual_docs->next());
  ASSERT_TRUE(irs::doc_limits::eof(expected_docs->value()));
  ASSERT_TRUE(irs::doc_limits::eof(actual_docs->value()));
}

void assert_terms_next(
    const irs::term_reader& expected_term_reader,
    const irs::term_reader& actual_term_reader,
    const irs::flags& features,
    irs::automaton_table_matcher* matcher) {
  irs::bytes_ref actual_min{ irs::bytes_ref::NIL };
  irs::bytes_ref actual_max{ irs::bytes_ref::NIL };
  irs::bstring actual_min_buf;
  irs::bstring actual_max_buf;
  size_t actual_size = 0;

  auto expected_term = matcher ? expected_term_reader.iterator(*matcher) : expected_term_reader.iterator();
  auto actual_term = matcher ? actual_term_reader.iterator(*matcher) : actual_term_reader.iterator();
  for (; expected_term->next(); ++actual_size) {
    ASSERT_TRUE(actual_term->next());

    assert_term(*expected_term, *actual_term, features);

    if (actual_min.null()) {
      actual_min_buf = actual_term->value();
      actual_min = actual_min_buf;
    }

    actual_max_buf = actual_term->value();
    actual_max = actual_max_buf;
  }
  //ASSERT_FALSE(actual_term->next()); // FIXME
  //ASSERT_FALSE(actual_term->next());

  // check term reader
  if (!matcher) {
    ASSERT_EQ(expected_term_reader.size(), actual_size);
    ASSERT_EQ((expected_term_reader.min)(), actual_min);
    ASSERT_EQ((expected_term_reader.max)(), actual_max);
  }
}

void assert_terms_seek(
    const irs::term_reader& expected_term_reader,
    const irs::term_reader& actual_term_reader,
    const irs::flags& features,
    irs::automaton_table_matcher* matcher,
    size_t lookahead /* = 10 */) {
  auto expected_term = matcher ? expected_term_reader.iterator(*matcher) : expected_term_reader.iterator();
  auto actual_term_with_state = matcher ? actual_term_reader.iterator(*matcher) : actual_term_reader.iterator();
  for (; expected_term->next();) {
    // seek with state
    {
      ASSERT_TRUE(actual_term_with_state->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term_with_state, features);
    }

    // seek without state, iterate forward
    irs::seek_term_iterator::seek_cookie::ptr cookie; // cookie
    {
      auto actual_term = actual_term_reader.iterator();
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);
      actual_term->read();
      cookie = actual_term->cookie();

      // iterate forward
      {
        auto copy_expected_term = expected_term_reader.iterator();
        ASSERT_TRUE(copy_expected_term->seek(expected_term->value()));
        ASSERT_EQ(expected_term->value(), copy_expected_term->value());
        for(size_t i = 0; i < lookahead; ++i) {
          const bool copy_expected_next = copy_expected_term->next();
          const bool actual_next = actual_term->next();
          ASSERT_EQ(copy_expected_next, actual_next);
          if (!copy_expected_next) {
            break;
          }
          assert_term(*copy_expected_term, *actual_term, features);
        }
      }
      
      // seek back to initial term
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);
    }

    // seek greater or equal without state, iterate forward
    {
      auto actual_term = actual_term_reader.iterator();
      ASSERT_EQ(irs::SeekResult::FOUND, actual_term->seek_ge(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);

      // iterate forward
      {
        auto copy_expected_term = expected_term_reader.iterator();
        ASSERT_TRUE(copy_expected_term->seek(expected_term->value()));
        ASSERT_EQ(expected_term->value(), copy_expected_term->value());
        for(size_t i = 0; i < lookahead; ++i) {
          const bool copy_expected_next = copy_expected_term->next();
          const bool actual_next = actual_term->next();
          ASSERT_EQ(copy_expected_next, actual_next);
          if (!copy_expected_next) {
            break;
          }
          assert_term(*copy_expected_term, *actual_term, features);
        }
      }
      
      // seek back to initial term
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);
    }

    // seek to cookie without state, iterate to the end
    {
      auto actual_term = actual_term_reader.iterator();
      ASSERT_TRUE(actual_term->seek(expected_term->value(), *cookie));
      ASSERT_EQ(expected_term->value(), actual_term->value());
      assert_term(*expected_term, *actual_term, features);

      // iterate forward
      {
        auto copy_expected_term = expected_term_reader.iterator();
        ASSERT_TRUE(copy_expected_term->seek(expected_term->value()));
        ASSERT_EQ(expected_term->value(), copy_expected_term->value());
        for(size_t i = 0; i < lookahead; ++i) {
          const bool copy_expected_next = copy_expected_term->next();
          const bool actual_next = actual_term->next();
          ASSERT_EQ(copy_expected_next, actual_next);
          if (!copy_expected_next) {
            break;
          }
          assert_term(*copy_expected_term, *actual_term, features);
        }
      }

      // seek to the same term
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);

      // seek to the same term
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);

      // seek greater equal to the same term
      ASSERT_EQ(irs::SeekResult::FOUND, actual_term->seek_ge(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);
    }
  }
}

void assert_index(
    const index_t& expected_index,
    const irs::index_reader& actual_index_reader,
    const irs::flags& features,
    size_t skip /*= 0*/,
    irs::automaton_table_matcher* matcher /*=nullptr*/) {
  // check number of segments
  ASSERT_EQ(expected_index.size(), actual_index_reader.size());
  size_t i = 0;
  for (auto& actual_sub_reader : actual_index_reader) {
    // skip segment if validation not required
    if (skip) {
      ++i;
      --skip;
      continue;
    }

    // setting up test reader/writer
    const tests::index_segment& expected_segment = expected_index[i];
    tests::field_reader expected_reader(expected_segment);

    // get field name iterators
    auto& expected_fields = expected_segment.fields();
    auto expected_fields_begin = expected_fields.begin();
    auto expected_fields_end = expected_fields.end();

    auto actual_fields = actual_sub_reader.fields();

    // iterate over fields
    for (;actual_fields->next(); ++expected_fields_begin) {
      // check field name
      ASSERT_EQ(expected_fields_begin->first, actual_fields->value().meta().name);

      // check field terms
      auto expected_term_reader = expected_reader.field(expected_fields_begin->second.name);
      ASSERT_NE(nullptr, expected_term_reader);
      auto actual_term_reader = actual_sub_reader.field(actual_fields->value().meta().name);
      ASSERT_NE(nullptr, actual_term_reader);

      const irs::field_meta* expected_field = expected_segment.find(expected_fields_begin->first);
      ASSERT_NE(nullptr, expected_field);

      // check term reader
      ASSERT_EQ((expected_term_reader->min)(), (actual_term_reader->min)());
      ASSERT_EQ((expected_term_reader->max)(), (actual_term_reader->max)());
      ASSERT_EQ(expected_term_reader->size(), actual_term_reader->size());
      ASSERT_EQ(expected_term_reader->docs_count(), actual_term_reader->docs_count());
      ASSERT_EQ(expected_term_reader->meta(), actual_term_reader->meta());

      auto& expected_attributes = expected_term_reader->attributes();
      auto& actual_attributes = actual_term_reader->attributes();
      auto expected_features = expected_term_reader->attributes().features();
      auto actual_features = actual_term_reader->attributes().features();
      ASSERT_EQ(expected_features, actual_features);

      if (expected_attributes.contains<irs::frequency>()) {
        auto& expected_freq = expected_attributes.get<irs::frequency>();
        auto& actual_freq = actual_attributes.get<irs::frequency>();
        ASSERT_TRUE(bool(actual_freq));
        ASSERT_EQ(expected_freq->value, actual_freq->value);
      }

      assert_terms_next(*expected_term_reader, *actual_term_reader, features, matcher);
      assert_terms_seek(*expected_term_reader, *actual_term_reader, features, matcher);
    }
    ++i;
    ASSERT_EQ(expected_fields_end, expected_fields_begin);
  }
}

void assert_index(
    const irs::directory& dir,
    irs::format::ptr codec,
    const index_t& expected_index,
    const irs::flags& features,
    size_t skip /*= 0*/,
    irs::automaton_table_matcher* matcher /*= nullptr*/) {
  auto actual_index_reader = irs::directory_reader::open(dir, codec);

  assert_index(expected_index, actual_index_reader, features, skip, matcher);
}

NS_END // tests

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
