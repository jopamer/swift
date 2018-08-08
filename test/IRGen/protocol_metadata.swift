// RUN: %target-swift-frontend -assume-parsing-unqualified-ownership-sil -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module -enable-objc-interop | %FileCheck %s -DINT=i%target-ptrsize

// REQUIRES: CPU=x86_64

protocol A { func a() }
protocol B { func b() }
protocol C : class { func c() }
@objc protocol O { func o() }
@objc protocol OPT { 
  @objc optional func opt()
  @objc optional static func static_opt()

  @objc optional var prop: O { get }
  @objc optional subscript (x: O) -> O { get }
}

protocol AB : A, B { func ab() }
protocol ABO : A, B, O { func abo() }

// -- @objc protocol O uses ObjC symbol mangling and layout
// CHECK-LABEL: @_PROTOCOL__TtP17protocol_metadata1O_ = private constant
// CHECK-SAME:   @_PROTOCOL_INSTANCE_METHODS__TtP17protocol_metadata1O_,
// -- size, flags: 1 = Swift
// CHECK-SAME:   i32 96, i32 1
// CHECK-SAME: @_PROTOCOL_METHOD_TYPES__TtP17protocol_metadata1O_
// CHECK-SAME: }

// CHECK: [[A_NAME:@.*]] = private constant [2 x i8] c"A\00"

// CHECK-LABEL: @"$S17protocol_metadata1AMp" = hidden constant
// CHECK-SAME:   i32 65603,
// CHECK-SAME:   @"$S17protocol_metadataMXM"
// CHECK-SAME:   [[A_NAME]]
// CHECK-SAME:   i32 0,
// CHECK-SAME:   i32 1,
// CHECK-SAME:   i32 0,
// CHECK-SAME: }

// CHECK: [[B_NAME:@.*]] = private constant [2 x i8] c"B\00"
// CHECK-LABEL: @"$S17protocol_metadata1BMp" = hidden constant
// CHECK-SAME:   i32 65603,
// CHECK-SAME:   @"$S17protocol_metadataMXM"
// CHECK-SAME:   i32 0,
// CHECK-SAME:   [[B_NAME]]
// CHECK-SAME:   i32 1,
// CHECK: }

// CHECK: [[C_NAME:@.*]] = private constant [2 x i8] c"C\00"
// CHECK-LABEL: @"$S17protocol_metadata1CMp" = hidden constant
// CHECK-SAME:   i32 67,
// CHECK-SAME:   @"$S17protocol_metadataMXM"
// CHECK-SAME:   [[C_NAME]]
// CHECK-SAME:   i32 1,
// CHECK-SAME:   i32 1,
// CHECK-SAME:   i32 0,
// AnyObject layout constraint
// CHECK-SAME:   i32 31, i32 0, i32 0
// CHECK-SAME: }

// -- @objc protocol OPT uses ObjC symbol mangling and layout
// CHECK: @_PROTOCOL__TtP17protocol_metadata3OPT_ = private constant { {{.*}} i32, [4 x i8*]*, i8*, i8* } {
// CHECK-SAME:   @_PROTOCOL_INSTANCE_METHODS_OPT__TtP17protocol_metadata3OPT_,
// CHECK-SAME:   @_PROTOCOL_CLASS_METHODS_OPT__TtP17protocol_metadata3OPT_,
// -- size, flags: 1 = Swift
// CHECK-SAME:   i32 96, i32 1
// CHECK-SAME: @_PROTOCOL_METHOD_TYPES__TtP17protocol_metadata3OPT_
// CHECK-SAME: }

// -- inheritance lists for refined protocols

// CHECK: [[AB_NAME:@.*]] = private constant [3 x i8] c"AB\00"
// CHECK: @"$S17protocol_metadata2ABMp" = hidden constant
// CHECK-SAME:   i32 65603,
// CHECK-SAME:   @"$S17protocol_metadataMXM"
// CHECK-SAME:   [[AB_NAME]]
// CHECK-SAME:   i32 2, i32 3, i32 0

// Inheritance from A
// CHECK-SAME:   i32 128, i32 0
// CHECK-SAME: @"$S17protocol_metadata1AMp"

// Inheritance from B
// CHECK-SAME:   i32 128, i32 0
// CHECK-SAME:   @"$S17protocol_metadata1BMp"
// CHECK: }

protocol Comprehensive {
  associatedtype Assoc : A
  init()
  func instanceMethod()
  static func staticMethod()
  var instance: Assoc { get set }
  static var global: Assoc { get set }
}

// CHECK: [[COMPREHENSIVE_ASSOC_NAME:@.*]] = private constant [6 x i8] c"Assoc\00"

// CHECK: @"$S17protocol_metadata13ComprehensiveMp" = hidden constant
// CHECK-SAME: i32 65603
// CHECK-SAME: i32 1
// CHECK-SAME: i32 11,
// CHECK-SAME: i32 trunc
// CHECK-SAME: [6 x i8]* [[COMPREHENSIVE_ASSOC_NAME]]
// CHECK-SAME:   %swift.protocol_requirement { i32 6, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 7, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 2, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 17, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 1, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 19, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 20, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 21, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 3, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 4, i32 0, i32 0 },
// CHECK-SAME:   %swift.protocol_requirement { i32 5, i32 0, i32 0 }


func reify_metadata<T>(_ x: T) {}

// CHECK: define hidden swiftcc void @"$S17protocol_metadata0A6_types{{[_0-9a-zA-Z]*}}F"
func protocol_types(_ a: A,
                    abc: A & B & C,
                    abco: A & B & C & O) {
  // CHECK: store [[INT]] ptrtoint ({{.*}} @"$S17protocol_metadata1AMp" to [[INT]])
  // CHECK: call %swift.type* @swift_getExistentialTypeMetadata(i1 true, %swift.type* null, i64 1, [[INT]]* {{%.*}})
  reify_metadata(a)
  // CHECK: store [[INT]] ptrtoint ({{.*}} @"$S17protocol_metadata1AMp"
  // CHECK: store [[INT]] ptrtoint ({{.*}} @"$S17protocol_metadata1BMp"
  // CHECK: store [[INT]] ptrtoint ({{.*}} @"$S17protocol_metadata1CMp"
  // CHECK: call %swift.type* @swift_getExistentialTypeMetadata(i1 false, %swift.type* null, i64 3, [[INT]]* {{%.*}})
  reify_metadata(abc)
  // CHECK: store [[INT]] ptrtoint ({{.*}} @"$S17protocol_metadata1AMp"
  // CHECK: store [[INT]] ptrtoint ({{.*}} @"$S17protocol_metadata1BMp"
  // CHECK: store [[INT]] ptrtoint ({{.*}} @"$S17protocol_metadata1CMp"
  // CHECK: [[O_REF:%.*]] = load i8*, i8** @"\01l_OBJC_PROTOCOL_REFERENCE_$__TtP17protocol_metadata1O_"
  // CHECK: [[O_REF_INT:%.*]] = ptrtoint i8* [[O_REF]] to [[INT]]
  // CHECK: [[O_REF_DESCRIPTOR:%.*]] = or [[INT]] [[O_REF_INT]], 1
  // CHECK: store [[INT]] [[O_REF_DESCRIPTOR]]
  // CHECK: call %swift.type* @swift_getExistentialTypeMetadata(i1 false, %swift.type* null, i64 4, [[INT]]* {{%.*}})
  reify_metadata(abco)
}

