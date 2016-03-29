//===--- Range.swift ------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// An iterator over the elements of `Range<Element>`.
public struct RangeIterator<
  Element : ForwardIndex
> : IteratorProtocol, Sequence {

  /// Construct an instance that traverses the elements of `bounds`.
  @_versioned
  @_transparent
  internal init(_bounds: Range<Element>) {
    self.startIndex = _bounds.startIndex
    self.endIndex = _bounds.endIndex
  }

  /// Advance to the next element and return it, or `nil` if no next
  /// element exists.
  @inline(__always)
  public mutating func next() -> Element? {
    if startIndex == endIndex { return nil }
    let element = startIndex
    startIndex._successorInPlace()
    return element
  }

  /// The lower bound of the remaining range.
  internal var startIndex: Element

  /// The upper bound of the remaining range; not included in the
  /// generated sequence.
  internal let endIndex: Element
}

/// A collection of consecutive discrete index values.
///
/// - parameter Element: Is both the element type and the index type of the
///   collection.
///
/// Like other collections, a range containing one element has an
/// `endIndex` that is the successor of its `startIndex`; and an empty
/// range has `startIndex == endIndex`.
///
/// Axiom: for any `Range` `r`, `r[i] == i`.
///
/// Therefore, if `Element` has a maximal value, it can serve as an
/// `endIndex`, but can never be contained in a `Range<Element>`.
///
/// It also follows from the axiom above that `(-99..<100)[0] == 0`.
/// To prevent confusion (because some expect the result to be `-99`),
/// in a context where `Element` is known to be an integer type,
/// subscripting with `Element` is a compile-time error:
///
///     // error: could not find an overload for 'subscript'...
///     print(Range<Int>(start: -99, end: 100)[0])
///
/// However, subscripting that range still works in a generic context:
///
///     func brackets<Element : ForwardIndex>(x: Range<Element>, i: Element) -> Element {
///       return x[i] // Just forward to subscript
///     }
///     print(brackets(Range<Int>(start: -99, end: 100), 0))
///     // Prints "0"
public struct Range<
  Element : ForwardIndex
> : Equatable, Collection,
    CustomStringConvertible, CustomDebugStringConvertible {

  /// Construct a copy of `x`.
  public init(_ x: Range) {
    // This initializer exists only so that we can have a
    // debugDescription that actually constructs the right type when
    // evaluated
    self = x
  }

  /// Construct a range with `startIndex == start` and `endIndex ==
  /// end`.
  @_transparent
  internal init(_start: Element, end: Element) {
    self.startIndex = _start
    self.endIndex = end
  }

  /// Access the element at `position`.
  ///
  /// - Precondition: `position` is a valid position in `self` and
  ///   `position != endIndex`.
  public subscript(position: Element) -> Element {
    _debugPrecondition(position != endIndex, "Index out of range")
    return position
  }

  //===--------------------------------------------------------------------===//
  // Overloads for subscript that allow us to make subscripting fail
  // at compile time, outside a generic context, when Element is an Integer
  // type. The current language design gives us no way to force r[0]
  // to work "as expected" (return the first element of the range) for
  // an arbitrary Range<Int>, so instead we make it ambiguous.  Same
  // goes for slicing.  The error message will be poor but at least it
  // is a compile-time error.
  public subscript(_: Element._DisabledRangeIndex) -> Element {
    _sanityCheckFailure("It shouldn't be possible to call this function'")
  }

  //===--------------------------------------------------------------------===//

  /// Returns an iterator over the elements of this sequence.
  ///
  /// - Complexity: O(1).
  @inline(__always)
  public func makeIterator() -> RangeIterator<Element> {
    return RangeIterator(_bounds: self)
  }

  /// The range's lower bound.
  ///
  /// Identical to `endIndex` in an empty range.
  public var startIndex: Element

  /// The range's upper bound.
  ///
  /// `endIndex` is not a valid argument to `subscript`, and is always
  /// reachable from `startIndex` by zero or more applications of
  /// `successor()`.
  public var endIndex: Element

  /// A textual representation of `self`.
  public var description: String {
    return "\(startIndex)..<\(endIndex)"
  }

  /// A textual representation of `self`, suitable for debugging.
  public var debugDescription: String {
    return "Range(\(String(reflecting: startIndex))..<\(String(reflecting: endIndex)))"
  }
}

extension Range : CustomReflectable {
  public var customMirror: Mirror {
    return Mirror(self, children: ["startIndex": startIndex, "endIndex": endIndex])
  }
}

/// O(1) implementation of `contains()` for ranges of comparable elements.
extension Range where Element : Comparable {
  @warn_unused_result
  public func _customContainsEquatableElement(element: Element) -> Bool? {
    return element >= self.startIndex && element < self.endIndex
  }

  // FIXME: copied from SequenceAlgorithms as a workaround for
  // https://bugs.swift.org/browse/SR-435
  @warn_unused_result
  public func contains(element: Element) -> Bool {
    if let result = _customContainsEquatableElement(element) {
      return result
    }

    for e in self {
      if e == element {
        return true
      }
    }
    return false
  }
}

@warn_unused_result
public func == <Element>(lhs: Range<Element>, rhs: Range<Element>) -> Bool {
  return lhs.startIndex == rhs.startIndex &&
      lhs.endIndex == rhs.endIndex
}

/// Forms a half-open range that contains `minimum`, but not
/// `maximum`.
@_transparent
@warn_unused_result
public func ..< <Pos : ForwardIndex> (minimum: Pos, maximum: Pos)
  -> Range<Pos> {
  return Range(_start: minimum, end: maximum)
}

/// Forms a closed range that contains both `minimum` and `maximum`.
@_transparent
@warn_unused_result
public func ... <Pos : ForwardIndex> (
  minimum: Pos, maximum: Pos
) -> Range<Pos> {
  return Range(_start: minimum, end: maximum.successor())
}

//===--- Prefer Ranges to Intervals, and add checking ---------------------===//

/// Forms a half-open range that contains `start`, but not `end`.
///
/// - Precondition: `start <= end`.
@_transparent
@warn_unused_result
public func ..< <Pos : ForwardIndex where Pos : Comparable> (
  start: Pos, end: Pos
) -> Range<Pos> {
  _precondition(start <= end, "Can't form Range with end < start")
  return Range(_start: start, end: end)
}

/// Forms a closed range that contains both `start` and `end`.
/// - Precondition: `start <= end`.
@_transparent
@warn_unused_result
public func ... <Pos : ForwardIndex where Pos : Comparable> (
  start: Pos, end: Pos
) -> Range<Pos> {
  _precondition(start <= end, "Can't form Range with end < start")
  _precondition(end.successor() > end, "Range end index has no valid successor")
  return Range(_start: start, end: end.successor())
}

@warn_unused_result
public func ~= <I : ForwardIndex where I : Comparable> (
  pattern: Range<I>, value: I
) -> Bool {
  return pattern.contains(value)
}

@available(*, unavailable, renamed: "RangeIterator")
public struct RangeGenerator<Element : ForwardIndex> {}

extension RangeIterator {
  @available(*, unavailable, message: "use the 'makeIterator()' method on the collection")
  public init(_ bounds: Range<Element>) {
    fatalError("unavailable function can't be called")
  }
}

extension Range {
  @available(*, unavailable, message: "use the '..<' operator")
  public init(start: Element, end: Element) {
    fatalError("unavailable function can't be called")
  }
}
