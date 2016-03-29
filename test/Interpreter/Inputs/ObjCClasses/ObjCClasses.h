#ifndef SWIFT_TEST_OBJC_CLASSES_H
#define SWIFT_TEST_OBJC_CLASSES_H

#import <Foundation/NSObject.h>

NS_ASSUME_NONNULL_BEGIN

/* This class has instance variables which are not apparent in the
   interface.  Subclasses will need to be slid by the ObjC runtime. */
@interface HasHiddenIvars : NSObject
@property NSInteger x;
@property NSInteger y;
@property NSInteger z;
@property NSInteger t;
@end

/* This class has a method that doesn't fill in the error properly. */
@interface NilError : NSObject
+ (BOOL) throwIt: (NSError**) error;
@end

@interface Container<C> : NSObject
- (id)initWithObject:(C)object NS_DESIGNATED_INITIALIZER;
- (id)init NS_UNAVAILABLE;

@property C object;

- (void)processObjectWithBlock:(void (^)(C))block;
- (void)updateObjectWithBlock:(C (^)())block;
@end

@interface Container<D> (Cat1)
- (id)initWithCat1:(D)object;
- (D)getCat1;
- (void)setCat1:(D)object;
@property D cat1Property;
@end

@interface SubContainer<E> : Container<E>
@end

@interface NestedContainer<F> : Container<Container<F> *>
@end

@interface StringContainer : Container<NSString *>
@end

@interface CopyingContainer<C: id<NSCopying>> : Container<C>
@end

@interface Animal : NSObject
@property (readonly) NSString *noise;
@end

@interface Dog : Animal
@end

@interface AnimalContainer<C: Animal *> : Container<C>
@end

NS_ASSUME_NONNULL_END

#endif
