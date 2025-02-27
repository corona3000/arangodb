////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "ConstFetcher.h"

#include "Aql/AqlCallStack.h"
#include "Aql/DependencyProxy.h"
#include "Aql/ShadowAqlItemRow.h"
#include "Aql/SkipResult.h"
#include "Basics/Exceptions.h"
#include "Basics/voc-errors.h"

using namespace arangodb;
using namespace arangodb::aql;

ConstFetcher::ConstFetcher() : _currentBlock{nullptr}, _rowIndex(0) {}

ConstFetcher::ConstFetcher(DependencyProxy& executionBlock)
    : _currentBlock{nullptr}, _rowIndex(0) {}

auto ConstFetcher::execute(AqlCallStack& stack)
    -> std::tuple<ExecutionState, SkipResult, AqlItemBlockInputRange> {
  // Note this fetcher can only be executed on top level (it is the singleton, or test)
  TRI_ASSERT(stack.isRelevant());
  // We only peek the call here, as we do not take over ownership.
  // We can replace this by pop again if all executors also only take a reference to the stack.
  auto call = stack.peek();
  if (_blockForPassThrough == nullptr) {
    // we are done, nothing to move arround here.
    return {ExecutionState::DONE, SkipResult{}, AqlItemBlockInputRange{ExecutorState::DONE}};
  }
  std::vector<std::pair<size_t, size_t>> sliceIndexes;
  sliceIndexes.emplace_back(_rowIndex, _blockForPassThrough->size());
  // Modifiable first slice indexes.
  // from is the first data row to be returned
  // to is one after the last data row to be returned

  if (_blockForPassThrough->hasShadowRows()) {
    auto shadowIndexes = _blockForPassThrough->getShadowRowIndexes();
    auto shadowRow = shadowIndexes.lower_bound(_rowIndex);
    if (shadowRow != shadowIndexes.end()) {
      size_t fromShadowRow = *shadowRow;
      size_t toShadowRow = *shadowRow + 1;
      for (++shadowRow; shadowRow != shadowIndexes.end(); ++shadowRow) {
        if (*shadowRow == toShadowRow) {
          ShadowAqlItemRow srow{_blockForPassThrough, toShadowRow};
          TRI_ASSERT(srow.isInitialized());
          if (srow.isRelevant()) {
            // we cannot jump over relveant shadow rows.
            // Unfortunately we need to stop including rows here.
            // NOTE: As all blocks have this behaviour anyway
            // this is not cirtical.
            break;
          }
          toShadowRow++;
        }
      }
      TRI_ASSERT(fromShadowRow < toShadowRow);
      // We cannot go past the first shadowRow
      sliceIndexes.emplace_back(fromShadowRow, toShadowRow);
      sliceIndexes[0].second = fromShadowRow;
    }
  }
  // Number of data rows we have left
  size_t rowsLeft = sliceIndexes[0].second - sliceIndexes[0].first;
  {
    // We use this scope here to ensure the correctness
    // of the following reference.
    // if sliceIndexes is modified the references are broken
    // in this scope it is ensured that sliceIndexes is not
    // modified in size.
    // These indexes will be modified by the call.
    auto& [from, to] = sliceIndexes[0];

    {
      // Skip over the front rows.
      // Adjust from and rowsLeft
      // Note: canSkip can be 0
      size_t canSkip = (std::min)(call.getOffset(), rowsLeft);
      from += canSkip;
      rowsLeft -= canSkip;
      call.didSkip(canSkip);
    }
    {
      // Produce the next rows
      // Adjost from and rowsLeft
      // Note: canProduce can be 0
      size_t canProduce = (std::min)(call.getLimit(), rowsLeft);
      to = from + canProduce;
      rowsLeft -= canProduce;
      call.didProduce(canProduce);
    }
  }

  // Now adjust the rowIndex for consumed rows
  if (call.hasHardLimit() && rowsLeft > 0) {
    // fast forward
    // We can only get here, if we have skipped and produced all rows
    TRI_ASSERT(call.getOffset() == 0 && call.getLimit() == 0);
    if (call.needsFullCount()) {
      call.didSkip(rowsLeft);
    }
    rowsLeft = 0;

    // In this case we consumed all rows until the end of
    // a) the shadowRow range
    // b) the end of the block
    if (sliceIndexes.size() == 2) {
      // We have shadowRows in use, go to end of their indexes
      _rowIndex = sliceIndexes.back().second;
    } else {
      // We do not have shadowRows in use, need to to go the end.
      _rowIndex = _blockForPassThrough->size();
    }
  } else {
    // No hardLimit, but softLimit.
    // And we have not reached the end.
    if (rowsLeft > 0 && sliceIndexes.size() == 2) {
      // Cannot include shadowRows
      sliceIndexes.pop_back();
    }
    // Row index is now at the end of the last returned row
    _rowIndex = sliceIndexes.back().second;
  }

  // Now we have a slicing vector:
  // [0] => <from, to> (data rows)
  // [1] => <from2, to2> (shadow rows, optional)
  TRI_ASSERT(sliceIndexes.size() == 1 || sliceIndexes.size() == 2);

  if (canUseFullBlock(sliceIndexes)) {
    // FastPath
    // No need for slicing
    SharedAqlItemBlockPtr resultBlock = _blockForPassThrough;
    _blockForPassThrough.reset(nullptr);
    _rowIndex = 0;
    SkipResult skipped{};
    skipped.didSkip(call.getSkipCount());

    return {ExecutionState::DONE, skipped,
            DataRange{ExecutorState::DONE, call.getSkipCount(), resultBlock, 0}};
  }

  SharedAqlItemBlockPtr resultBlock = _blockForPassThrough;

  if (_rowIndex >= resultBlock->size()) {
    // used the full block by now.
    _blockForPassThrough.reset(nullptr);
  }

  if (sliceIndexes[0].first >= sliceIndexes[0].second) {
    // We do not return any DataRow either we do not have one,
    // or we have completely skipped it.
    // Remove the indexes from the slice list
    sliceIndexes.erase(sliceIndexes.begin());
  }
  // NOTE: The above if may have invalidated from and to memory.
  // Do not use them below this point!

  if (sliceIndexes.empty()) {
    // No data to be returned
    // Block is dropped.
    resultBlock = nullptr;
    SkipResult skipped{};
    skipped.didSkip(call.getSkipCount());
    return {ExecutionState::DONE, skipped,
            DataRange{ExecutorState::DONE, call.getSkipCount()}};
  }

  // Slowest path need to slice, this unfortunately requires copy of data
  ExecutionState resState =
      _blockForPassThrough == nullptr ? ExecutionState::DONE : ExecutionState::HASMORE;
  ExecutorState rangeState =
      _blockForPassThrough == nullptr ? ExecutorState::DONE : ExecutorState::HASMORE;

  resultBlock = resultBlock->slice(sliceIndexes);
  SkipResult skipped{};
  skipped.didSkip(call.getSkipCount());
  return {resState, skipped, DataRange{rangeState, call.getSkipCount(), resultBlock, 0}};
}

void ConstFetcher::injectBlock(SharedAqlItemBlockPtr block) {
  _currentBlock = block;
  _blockForPassThrough = std::move(block);
  _rowIndex = 0;
}

std::pair<ExecutionState, InputAqlItemRow> ConstFetcher::fetchRow(size_t) {
  // This fetcher does not use atMost
  // This fetcher never waits because it can return only its
  // injected block and does not have the ability to pull.
  if (!indexIsValid()) {
    return {ExecutionState::DONE, InputAqlItemRow{CreateInvalidInputRowHint{}}};
  }
  TRI_ASSERT(_currentBlock != nullptr);

  // set state
  ExecutionState rowState = ExecutionState::HASMORE;
  if (isLastRowInBlock()) {
    rowState = ExecutionState::DONE;
  }

  return {rowState, InputAqlItemRow{_currentBlock, _rowIndex++}};
}

std::pair<ExecutionState, size_t> ConstFetcher::skipRows(size_t) {
  // This fetcher never waits because it can return only its
  // injected block and does not have the ability to pull.
  if (!indexIsValid()) {
    return {ExecutionState::DONE, 0};
  }
  TRI_ASSERT(_currentBlock != nullptr);

  // set state
  ExecutionState rowState = ExecutionState::HASMORE;
  if (isLastRowInBlock()) {
    rowState = ExecutionState::DONE;
  }
  _rowIndex++;

  return {rowState, 1};
}

auto ConstFetcher::indexIsValid() const noexcept -> bool {
  return _currentBlock != nullptr && _rowIndex + 1 <= _currentBlock->size();
}

auto ConstFetcher::isLastRowInBlock() const noexcept -> bool {
  TRI_ASSERT(indexIsValid());
  return _rowIndex + 1 == _currentBlock->size();
}

auto ConstFetcher::numRowsLeft() const noexcept -> size_t {
  if (!indexIsValid()) {
    return 0;
  }
  return _currentBlock->size() - _rowIndex;
}

auto ConstFetcher::canUseFullBlock(std::vector<std::pair<size_t, size_t>> const& ranges) const
    noexcept -> bool {
  TRI_ASSERT(!ranges.empty());
  if (ranges.front().first != 0) {
    // We do not start at the first index.
    return false;
  }
  if (ranges.back().second != _currentBlock->size()) {
    // We de not stop at the last index
    return false;
  }

  if (ranges.size() > 1) {
    TRI_ASSERT(ranges.size() == 2);
    if (ranges.front().second != ranges.back().first) {
      // We have two ranges, that are not next to each other.
      // We cannot use the full block, as we need to slice these out.
      return false;
    }
  }
  // If we get here, the ranges covers the full block
  return true;
}

std::pair<ExecutionState, SharedAqlItemBlockPtr> ConstFetcher::fetchBlockForPassthrough(size_t) {
  // Should only be called once, and then _blockForPassThrough should be
  // initialized. However, there are still some blocks left that ask their
  // parent even after they got DONE the last time, and I don't currently have
  // time to track them down. Thus the following assert is commented out.
  // TRI_ASSERT(_blockForPassThrough != nullptr);
  return {ExecutionState::DONE, std::move(_blockForPassThrough)};
}

std::pair<ExecutionState, ShadowAqlItemRow> ConstFetcher::fetchShadowRow(size_t) const {
  return {ExecutionState::DONE, ShadowAqlItemRow{CreateInvalidShadowRowHint{}}};
}
