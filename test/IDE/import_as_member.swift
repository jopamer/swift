// RUN: %target-swift-ide-test(mock-sdk: %clang-importer-sdk) -I %t -I %S/Inputs/custom-modules -print-module -source-filename %s -module-to-print=ImportAsMember.A -always-argument-labels > %t.printed.A.txt
// RUN: %target-swift-ide-test(mock-sdk: %clang-importer-sdk) -I %t -I %S/Inputs/custom-modules -print-module -source-filename %s -module-to-print=ImportAsMember.B -always-argument-labels > %t.printed.B.txt
// RUN: %target-swift-ide-test(mock-sdk: %clang-importer-sdk) -I %t -I %S/Inputs/custom-modules -print-module -source-filename %s -module-to-print=ImportAsMember.Proto -always-argument-labels > %t.printed.Proto.txt

// RUN: FileCheck %s -check-prefix=PRINT -strict-whitespace < %t.printed.A.txt
// RUN: FileCheck %s -check-prefix=PRINTB -strict-whitespace < %t.printed.B.txt
// RUN: FileCheck %s -check-prefix=PRINT-PROTO -strict-whitespace < %t.printed.Proto.txt

// PRINT: struct Struct1 {
// PRINT-NEXT:   var x: Double
// PRINT-NEXT:   var y: Double
// PRINT-NEXT:   var z: Double
// PRINT-NEXT:   init()
// PRINT-NEXT:   init(x x: Double, y y: Double, z z: Double)
// PRINT-NEXT: }

// Make sure the other extension isn't here.
// PRINT-NOT: static var static1: Double

// PRINT:      extension Struct1 {
// PRINT-NEXT:   static var globalVar: Double
// PRINT-NEXT:   init(value value: Double)
// PRINT-NEXT:   func inverted() -> Struct1
// PRINT-NEXT:   mutating func invert()
// PRINT-NEXT:   func translate(radians radians: Double) -> Struct1
// PRINT-NEXT:   func scale(_ radians: Double) -> Struct1
// PRINT-NEXT:   var radius: Double { get nonmutating set }
// PRINT-NEXT:   var altitude: Double{{$}}
// PRINT-NEXT:   var magnitude: Double { get }
// PRINT-NEXT:   static func staticMethod() -> Int32
// PRINT-NEXT:   static var property: Int32
// PRINT-NEXT:   static var getOnlyProperty: Int32 { get }
// PRINT-NEXT:   func selfComesLast(x x: Double)
// PRINT-NEXT:   func selfComesThird(a a: Int32, b b: Float, x x: Double)
// PRINT-NEXT: }
// PRINT-NOT: static var static1: Double


// Make sure the other extension isn't here.
// PRINTB-NOT: static var globalVar: Double

// PRINTB:      extension Struct1 {
// PRINTB:        static var static1: Double
// PRINTB-NEXT:   static var static2: Float
// PRINTB-NEXT:   init(float value: Float)
// PRINTB-NEXT:   static var zero: Struct1 { get }
// PRINTB-NEXT: }

// PRINTB: var currentStruct1: Struct1

// PRINTB-NOT: static var globalVar: Double

// PRINT-PROTO-LABEL: protocol ImportedProtocolBase : NSObjectProtocol {
// PRINT-PROTO-NEXT:  }
// PRINT-PROTO-NEXT:  typealias ImportedProtocolBase_t = ImportedProtocolBase
// PRINT-PROTO-NEXT:  protocol IAMProto : ImportedProtocolBase {
// PRINT-PROTO-NEXT:  }
// PRINT-PROTO-NEXT:  typealias IAMProto_t = IAMProto
// PRINT-PROTO-NEXT:  extension IAMProto {
// PRINT-PROTO-NEXT:    func mutateSomeState()
// PRINT-PROTO-NEXT:    func mutateSomeState(otherProto other: IAMProto_t!)
// PRINT-PROTO-NEXT:    var someValue: Int32
// PRINT-PROTO-NEXT:  }

// RUN: %target-parse-verify-swift -I %S/Inputs/custom-modules
// RUN: %target-swift-frontend %s -parse -I %S/Inputs/custom-modules -verify

// REQUIRES: objc_interop

import Foundation
import ImportAsMember.A
import ImportAsMember.B
import ImportAsMember.Proto
import IAMError

let iamStructFail = IAMStruct1CreateSimple()
  // expected-error@-1{{use of unresolved identifier 'IAMStruct1CreateSimple'}}
var iamStruct = Struct1(x: 1.0, y: 1.0, z: 1.0)

let gVarFail = IAMStruct1GlobalVar
  // expected-error@-1{{use of unresolved identifier 'IAMStruct1GlobalVar'}}
let gVar = Struct1.globalVar
print("\(gVar)")

let iamStructInitFail = IAMStruct1CreateSimple(42)
  // expected-error@-1{{use of unresolved identifier 'IAMStruct1CreateSimple'}}
let iamStructInitFail = Struct1(value: 42)

let gVar2 = Struct1.static2

// Instance properties
iamStruct.radius += 1.5
_ = iamStruct.magnitude

// Static properties
iamStruct = Struct1.zero

// Global properties
currentStruct1.x += 1.5

ErrorStruct.hasPrototype();
ErrorStruct.nonPrototype();
  // expected-error@-1{{type 'ErrorStruct' has no member 'nonPrototype'}}

// Protocols
@objc class Foo : NSObject, IAMProto {}

struct Bar : IAMProto {}
  // expected-error@-1{{non-class type 'Bar' cannot conform to class protocol 'IAMProto'}}
  // expected-error@-2{{non-class type 'Bar' cannot conform to class protocol 'ImportedProtocolBase'}}
  // expected-error@-3{{non-class type 'Bar' cannot conform to class protocol 'NSObjectProtocol'}}

@objc class FooErr : NSObject, ErrorProto {}

let foo = Foo()
foo.mutateSomeState()

let fooErr = FooErr()
fooErr.mutateSomeInstanceState()
FooErr.mutateSomeStaticState()
  // expected-error@-1{{type 'FooErr' has no member 'mutateSomeStaticState'}}

