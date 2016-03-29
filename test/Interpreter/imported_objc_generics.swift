// RUN: rm -rf %t
// RUN: mkdir -p %t
//
// RUN: %target-clang -fobjc-arc %S/Inputs/ObjCClasses/ObjCClasses.m -c -o %t/ObjCClasses.o
// RUN: %target-build-swift -Xfrontend -enable-import-objc-generics -I %S/Inputs/ObjCClasses/ %t/ObjCClasses.o %s -o %t/a.out
// RUN: %target-run %t/a.out

// REQUIRES: executable_test
// XFAIL: interpret
// REQUIRES: objc_interop

import Foundation
import StdlibUnittest
import ObjCClasses

var ImportedObjCGenerics = TestSuite("ImportedObjCGenerics")

ImportedObjCGenerics.test("Creation") {
  let cs = Container<NSString>(object: "i-just-met-you")
  expectEqual("i-just-met-you", cs.object)
  expectTrue(cs.dynamicType === Container<NSString>.self)
  expectTrue(cs.dynamicType === Container<AnyObject>.self)
}

ImportedObjCGenerics.test("Blocks") {
  let cs = Container<NSString>(object: "and-this-is-crazy")

  var fromBlock: NSString = ""
  cs.processObjectWithBlock { fromBlock = $0 }
  expectEqual("and-this-is-crazy", fromBlock)

  cs.updateObjectWithBlock { "but-heres-my-number" }
  expectEqual("but-heres-my-number", cs.object)
}

ImportedObjCGenerics.test("Categories") {
  let cs = Container<NSString>(cat1: "so-call-me-maybe")
  expectEqual("so-call-me-maybe", cs.getCat1())

  cs.setCat1("its-hard-to-look-right")
  expectEqual("its-hard-to-look-right", cs.cat1Property)
}

ImportedObjCGenerics.test("Subclasses") {
  let subContainer = SubContainer<NSString>(object: "at-you-baby")
  expectEqual("at-you-baby", subContainer.object)

  let nestedContainer = NestedContainer<NSString>(object: Container(object: "but-heres-my-number"))
  expectEqual("but-heres-my-number", nestedContainer.object.object)

  let stringContainer = StringContainer(object: "so-call-me-maybe")
  expectEqual("so-call-me-maybe", stringContainer.object)
}

ImportedObjCGenerics.test("SwiftGenerics") {
  func openContainer<T: AnyObject>(x: Container<T>) -> T {
    return x.object
  }
  func openStringContainer<T: Container<NSString>>(x: T) -> NSString {
    return x.object
  }
  func openArbitraryContainer<S: AnyObject, T: Container<S>>(x: T) -> S {
    return x.object
  }

  let scs = SubContainer<NSString>(object: "before-you-came-into-my-life")
  expectEqual("before-you-came-into-my-life", openContainer(scs))
  expectEqual("before-you-came-into-my-life", openStringContainer(scs))
  expectEqual("before-you-came-into-my-life", openArbitraryContainer(scs))

  let cs = Container<NSString>(object: "i-missed-you-so-bad")
  expectEqual("i-missed-you-so-bad", openContainer(cs))
  expectEqual("i-missed-you-so-bad", openStringContainer(cs))
  expectEqual("i-missed-you-so-bad", openArbitraryContainer(cs))

  let strContainer = SubContainer<NSString>(object: "i-missed-you-so-so-bad")
  expectEqual("i-missed-you-so-so-bad", openContainer(strContainer))
  expectEqual("i-missed-you-so-so-bad", openStringContainer(strContainer))
  expectEqual("i-missed-you-so-so-bad", openArbitraryContainer(strContainer))

  let numContainer = Container<NSNumber>(object: NSNumber(integer: 21))
  expectEqual(NSNumber(integer: 21), openContainer(numContainer))
  expectEqual(NSNumber(integer: 21), openArbitraryContainer(numContainer))

  let subNumContainer = SubContainer<NSNumber>(object: NSNumber(integer: 22))
  expectEqual(NSNumber(integer: 22), openContainer(subNumContainer))
  expectEqual(NSNumber(integer: 22), openArbitraryContainer(subNumContainer))
}

ImportedObjCGenerics.test("SwiftGenerics/Creation") {
  func makeContainer<T: AnyObject>(x: T) -> Container<T> {
    return Container(object: x)
  }

  // TODO: fix IRGen failure below
  //let c = makeContainer(NSNumber(integer: 22))
  //expectEqual(NSNumber(integer: 22), c.object)
}

ImportedObjCGenerics.test("ProtocolConstraints") {
  func copyContainerContents<T: NSCopying>(x: CopyingContainer<T>) -> T {
    return x.object.copyWithZone(nil) as! T
  }

  let cs = CopyingContainer<NSString>(object: "Happy 2012")
  expectEqual("Happy 2012", copyContainerContents(cs))
}

ImportedObjCGenerics.test("ClassConstraints") {
  func makeContainedAnimalMakeNoise<T>(x: AnimalContainer<T>) -> NSString {
    return x.object.noise
  }
  let petCarrier = AnimalContainer(object: Dog())
  expectEqual("woof", makeContainedAnimalMakeNoise(petCarrier))
}

runAllTests()
