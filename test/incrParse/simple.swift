// RUN: %empty-directory(%t)
// RUN: %incparse-test %s --test-case REPLACE
// RUN: %incparse-test %s --test-case REPLACE_BY_LONGER
// RUN: %incparse-test %s --test-case REPLACE_BY_SHORTER
// RUN: %incparse-test %s --test-case INSERT
// RUN: %incparse-test %s --test-case REMOVE
// RUN: %incparse-test %s --test-case ATTACH_TO_PREV_NODE
// RUN: %incparse-test %s --test-case CLASS_SURROUNDING
// RUN: %incparse-test %s --test-case MULTI_EDIT
// RUN: %incparse-test %s --test-case MULTI_EDIT_SAME_LINE
// RUN: %incparse-test %s --test-case REPLACE_WITH_MULTI_BYTE_CHAR
// RUN: %incparse-test %s --test-case REPLACE_MULTI_BYTE_CHAR_WITH_SHORTER
// RUN: %incparse-test %s --test-case LAST_CHARACTER_OF_STRUCT
// RUN: %incparse-test %s --test-case ADD_ARRAY_CLOSE_BRACKET
// RUN: %incparse-test %s --test-case ADD_IF_OPEN_BRACE

func start() {}

<reparse REPLACE>
func foo() {
}

_ = <<REPLACE<6|||7>>></reparse REPLACE>
_ = <<REPLACE_BY_LONGER<6|||"Hello World">>>
_ = <<REPLACE_BY_SHORTER<"Hello again"|||"a">>>
<<INSERT<|||foo()>>>
<<REMOVE<print("abc")|||>>>
foo()
<<ATTACH_TO_PREV_NODE<|||{}>>>
_ = 1

<<CLASS_SURROUNDING<|||class C {>>>
  func method1() {}

<<MULTI_EDIT<|||class C {>>>
  func method1() {}
<<MULTI_EDIT<|||}>>>

<<MULTI_EDIT_SAME_LINE<_|||let x>>> = <<MULTI_EDIT_SAME_LINE<1|||"hi">>>

let x = "<<REPLACE_WITH_MULTI_BYTE_CHAR<a|||👨‍👩‍👧‍👦>>>"
let x = "<<REPLACE_MULTI_BYTE_CHAR_WITH_SHORTER<👨‍👩‍👧‍👦|||🎉>>>"

private struc<<LAST_CHARACTER_OF_STRUCT<|||t>>> MyStruct {
}

var computedVar: [Int] {
  return [1
  <<ADD_ARRAY_CLOSE_BRACKET<|||]>>>
}

if true <<ADD_IF_OPEN_BRACE<|||{>>>
  _ = 5
}
