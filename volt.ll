target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@.str = private unnamed_addr constant [5 x i8] c"true\00", align 1
@.strlit = private unnamed_addr constant { ptr, i64 } { ptr @.str, i64 4 }
@.str.1 = private unnamed_addr constant [6 x i8] c"false\00", align 1
@.strlit.2 = private unnamed_addr constant { ptr, i64 } { ptr @.str.1, i64 5 }
@volt.exc.tag = internal thread_local global i32 -1
@.str.3 = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@.strlit.4 = private unnamed_addr constant { ptr, i64 } { ptr @.str.3, i64 1 }
@.str.5 = private unnamed_addr constant [22 x i8] c"Unhandled exception: \00", align 1
@.strlit.6 = private unnamed_addr constant { ptr, i64 } { ptr @.str.5, i64 21 }
@.str.7 = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@.strlit.8 = private unnamed_addr constant { ptr, i64 } { ptr @.str.7, i64 1 }
@.str.9 = private unnamed_addr constant [1 x i8] zeroinitializer, align 1
@.strlit.10 = private unnamed_addr constant { ptr, i64 } { ptr @.str.9, i64 0 }
@.str.11 = private unnamed_addr constant [8 x i8] c"  from \00", align 1
@.strlit.12 = private unnamed_addr constant { ptr, i64 } { ptr @.str.11, i64 7 }
@.str.13 = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@.strlit.14 = private unnamed_addr constant { ptr, i64 } { ptr @.str.13, i64 1 }
@volt.exc.storage = internal thread_local global [48 x i8] zeroinitializer, align 8

define i32 @"_V4Bool3<=>"(i1 %self, i1 %other) {
entry:
  %other1 = alloca i1, align 1
  store i1 %other, ptr %other1, align 1
  %0 = load i1, ptr %other1, align 1
  %1 = icmp eq i1 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 0

if.else:                                          ; preds = %entry
  %2 = xor i1 %self, true
  br i1 %2, label %and.rhs, label %and.end

if.end:                                           ; preds = %if.end4
  ret i32 0

and.rhs:                                          ; preds = %if.else
  %3 = load i1, ptr %other1, align 1
  br label %and.end

and.end:                                          ; preds = %and.rhs, %if.else
  %logic = phi i1 [ false, %if.else ], [ %3, %and.rhs ]
  br i1 %logic, label %if.then2, label %if.else3

if.then2:                                         ; preds = %and.end
  ret i32 -1

if.else3:                                         ; preds = %and.end
  ret i32 1

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i1 @"_V4Bool1!"(i1 %self) {
entry:
  br i1 %self, label %ternary.then, label %ternary.else

ternary.then:                                     ; preds = %entry
  br label %ternary.end

ternary.else:                                     ; preds = %entry
  br label %ternary.end

ternary.end:                                      ; preds = %ternary.else, %ternary.then
  %ternary = phi i1 [ false, %ternary.then ], [ true, %ternary.else ]
  ret i1 %ternary
}

define { ptr, i64 } @_V4Bool9to_string(i1 %self) {
entry:
  br i1 %self, label %ternary.then, label %ternary.else

ternary.then:                                     ; preds = %entry
  br label %ternary.end

ternary.else:                                     ; preds = %entry
  br label %ternary.end

ternary.end:                                      ; preds = %ternary.else, %ternary.then
  %ternary = phi ptr [ @.strlit, %ternary.then ], [ @.strlit.2, %ternary.else ]
  %0 = load { ptr, i64 }, ptr %ternary, align 8
  ret { ptr, i64 } %0
}

define i32 @"_V4Char3<=>"(i8 %self, i8 %other) {
entry:
  %other1 = alloca i8, align 1
  store i8 %other, ptr %other1, align 1
  %0 = load i8, ptr %other1, align 1
  %1 = icmp ult i8 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i8, ptr %other1, align 1
  %3 = icmp ugt i8 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V4Char4hash(i8 %self) {
entry:
  %0 = zext i8 %self to i64
  ret i64 %0
}

define { ptr, i64 } @_V4Char9to_string(i8 %self) {
entry:
  %new = alloca { ptr, i64 }, align 8
  %buf = alloca ptr, align 8
  %0 = call ptr @_V7Pointer6mallocI5UInt8E(i64 2)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %entry
  store ptr %0, ptr %buf, align 8
  %1 = load ptr, ptr %buf, align 8
  store i8 %self, ptr %1, align 1
  %2 = load ptr, ptr %buf, align 8
  %3 = getelementptr i8, ptr %2, i64 1
  store i8 0, ptr %3, align 1
  %4 = load ptr, ptr %buf, align 8
  call void @_V6String10initialize(ptr %new, ptr %4, i64 1)
  %exc.tag1 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending2 = icmp ne i32 %exc.tag1, -1
  br i1 %exc.pending2, label %exc.propagate3, label %exc.cont4

exc.propagate3:                                   ; preds = %exc.cont
  ret { ptr, i64 } zeroinitializer

exc.cont4:                                        ; preds = %exc.cont
  %5 = load { ptr, i64 }, ptr %new, align 8
  ret { ptr, i64 } %5
}

define void @_V9Exception10initialize(ptr %self, ptr %message, i32 %max_frames) {
entry:
  %call.result = alloca { ptr, i64, i64 }, align 8
  %max_frames1 = alloca i32, align 4
  %field.message = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 0
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %field.message, ptr align 8 %message, i64 16, i1 false)
  store i32 %max_frames, ptr %max_frames1, align 4
  %field.max_frames = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 2
  store i32 %max_frames, ptr %field.max_frames, align 4
  %0 = call { ptr, i64, i64 } @_V9Exception17capture_backtrace(ptr %self)
  store { ptr, i64, i64 } %0, ptr %call.result, align 8
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  %field.backtrace = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 1
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %field.backtrace, ptr align 8 %call.result, i64 24, i1 false)
  ret void
}

define { ptr, i64 } @_V9Exception7inspect(ptr %self) {
entry:
  %call.result11 = alloca { ptr, i64 }, align 8
  %call.result6 = alloca { ptr, i64 }, align 8
  %call.result1 = alloca { ptr, i64 }, align 8
  %call.result = alloca { ptr, i64 }, align 8
  %field.message = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 0
  %0 = call { ptr, i64 } @_V6String9to_string(ptr %field.message)
  store { ptr, i64 } %0, ptr %call.result, align 8
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %entry
  %1 = call { ptr, i64 } @"_V6String1+"(ptr %call.result, ptr @.strlit.4)
  store { ptr, i64 } %1, ptr %call.result1, align 8
  %exc.tag2 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending3 = icmp ne i32 %exc.tag2, -1
  br i1 %exc.pending3, label %exc.propagate4, label %exc.cont5

exc.propagate4:                                   ; preds = %exc.cont
  ret { ptr, i64 } zeroinitializer

exc.cont5:                                        ; preds = %exc.cont
  %2 = call { ptr, i64 } @_V9Exception16format_backtrace(ptr %self)
  store { ptr, i64 } %2, ptr %call.result6, align 8
  %exc.tag7 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending8 = icmp ne i32 %exc.tag7, -1
  br i1 %exc.pending8, label %exc.propagate9, label %exc.cont10

exc.propagate9:                                   ; preds = %exc.cont5
  ret { ptr, i64 } zeroinitializer

exc.cont10:                                       ; preds = %exc.cont5
  %3 = call { ptr, i64 } @"_V6String1+"(ptr %call.result1, ptr %call.result6)
  store { ptr, i64 } %3, ptr %call.result11, align 8
  %exc.tag12 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending13 = icmp ne i32 %exc.tag12, -1
  br i1 %exc.pending13, label %exc.propagate14, label %exc.cont15

exc.propagate14:                                  ; preds = %exc.cont10
  ret { ptr, i64 } zeroinitializer

exc.cont15:                                       ; preds = %exc.cont10
  %4 = load { ptr, i64 }, ptr %call.result11, align 8
  ret { ptr, i64 } %4
}

define { ptr, i64 } @_V9Exception9to_string(ptr %self) {
entry:
  %field.message = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 0
  %0 = load { ptr, i64 }, ptr %field.message, align 8
  ret { ptr, i64 } %0
}

define { ptr, i64, i64 } @_V9Exception17capture_backtrace(ptr %self) {
entry:
  %call.result = alloca { ptr, i64 }, align 8
  %raw_sym = alloca ptr, align 8
  %i = alloca i32, align 4
  %symbols = alloca ptr, align 8
  %frames = alloca { ptr, i64, i64 }, align 8
  %new = alloca { ptr, i64, i64 }, align 8
  %frames_count = alloca i32, align 4
  %buffer = alloca ptr, align 8
  %effective_max = alloca i32, align 4
  %field.max_frames = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 2
  %0 = load i32, ptr %field.max_frames, align 4
  %1 = icmp sgt i32 %0, 0
  br i1 %1, label %ternary.then, label %ternary.else

ternary.then:                                     ; preds = %entry
  %field.max_frames1 = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 2
  %2 = load i32, ptr %field.max_frames1, align 4
  br label %ternary.end

ternary.else:                                     ; preds = %entry
  br label %ternary.end

ternary.end:                                      ; preds = %ternary.else, %ternary.then
  %ternary = phi i32 [ %2, %ternary.then ], [ 64, %ternary.else ]
  store i32 %ternary, ptr %effective_max, align 4
  %3 = load i32, ptr %effective_max, align 4
  %4 = mul i32 %3, 8
  %5 = zext i32 %4 to i64
  %6 = call ptr @_V7Pointer6mallocI7PointerIEE(i64 %5)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %ternary.end
  ret { ptr, i64, i64 } zeroinitializer

exc.cont:                                         ; preds = %ternary.end
  store ptr %6, ptr %buffer, align 8
  %7 = load ptr, ptr %buffer, align 8
  %8 = load i32, ptr %effective_max, align 4
  %9 = call i32 @backtrace(ptr %7, i32 %8)
  store i32 %9, ptr %frames_count, align 4
  %10 = load i32, ptr %frames_count, align 4
  %11 = zext i32 %10 to i64
  call void @_V5Array10initializeI6StringE(ptr %new, i64 %11)
  %exc.tag2 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending3 = icmp ne i32 %exc.tag2, -1
  br i1 %exc.pending3, label %exc.propagate4, label %exc.cont5

exc.propagate4:                                   ; preds = %exc.cont
  ret { ptr, i64, i64 } zeroinitializer

exc.cont5:                                        ; preds = %exc.cont
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %frames, ptr align 8 %new, i64 24, i1 false)
  %12 = load i32, ptr %frames_count, align 4
  %13 = icmp sgt i32 %12, 0
  br i1 %13, label %if.then, label %if.end

if.then:                                          ; preds = %exc.cont5
  %14 = load ptr, ptr %buffer, align 8
  %15 = load i32, ptr %frames_count, align 4
  %16 = call ptr @backtrace_symbols(ptr %14, i32 %15)
  store ptr %16, ptr %symbols, align 8
  %17 = load ptr, ptr %symbols, align 8
  %18 = icmp ne ptr %17, null
  br i1 %18, label %if.then6, label %if.end7

if.end:                                           ; preds = %if.end7, %exc.cont5
  %19 = load ptr, ptr %buffer, align 8
  call void @_V7Pointer4freeI7PointerIEE(ptr %19)
  %exc.tag22 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending23 = icmp ne i32 %exc.tag22, -1
  br i1 %exc.pending23, label %exc.propagate24, label %exc.cont25

if.then6:                                         ; preds = %if.then
  store i32 0, ptr %i, align 4
  br label %while.cond

if.end7:                                          ; preds = %exc.cont21, %if.then
  br label %if.end

while.cond:                                       ; preds = %if.end9, %if.then6
  %20 = load i32, ptr %i, align 4
  %21 = load i32, ptr %frames_count, align 4
  %22 = icmp slt i32 %20, %21
  br i1 %22, label %while.body, label %while.end

while.body:                                       ; preds = %while.cond
  %23 = load ptr, ptr %symbols, align 8
  %24 = load i32, ptr %i, align 4
  %25 = zext i32 %24 to i64
  %26 = getelementptr ptr, ptr %23, i64 %25
  %27 = load ptr, ptr %26, align 8
  store ptr %27, ptr %raw_sym, align 8
  %28 = load ptr, ptr %raw_sym, align 8
  %29 = icmp ne ptr %28, null
  br i1 %29, label %if.then8, label %if.end9

while.end:                                        ; preds = %while.cond
  %30 = load ptr, ptr %symbols, align 8
  call void @_V7Pointer4freeI7PointerI5UInt8EE(ptr %30)
  %exc.tag18 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending19 = icmp ne i32 %exc.tag18, -1
  br i1 %exc.pending19, label %exc.propagate20, label %exc.cont21

if.then8:                                         ; preds = %while.body
  %31 = load ptr, ptr %raw_sym, align 8
  %32 = call { ptr, i64 } @_V6String13from_c_string(ptr %31)
  store { ptr, i64 } %32, ptr %call.result, align 8
  %exc.tag10 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending11 = icmp ne i32 %exc.tag10, -1
  br i1 %exc.pending11, label %exc.propagate12, label %exc.cont13

if.end9:                                          ; preds = %exc.cont17, %while.body
  %33 = load i32, ptr %i, align 4
  %34 = add i32 %33, 1
  store i32 %34, ptr %i, align 4
  br label %while.cond

exc.propagate12:                                  ; preds = %if.then8
  ret { ptr, i64, i64 } zeroinitializer

exc.cont13:                                       ; preds = %if.then8
  call void @_V5Array4pushI6StringE(ptr %frames, ptr %call.result)
  %exc.tag14 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending15 = icmp ne i32 %exc.tag14, -1
  br i1 %exc.pending15, label %exc.propagate16, label %exc.cont17

exc.propagate16:                                  ; preds = %exc.cont13
  ret { ptr, i64, i64 } zeroinitializer

exc.cont17:                                       ; preds = %exc.cont13
  br label %if.end9

exc.propagate20:                                  ; preds = %while.end
  ret { ptr, i64, i64 } zeroinitializer

exc.cont21:                                       ; preds = %while.end
  br label %if.end7

exc.propagate24:                                  ; preds = %if.end
  ret { ptr, i64, i64 } zeroinitializer

exc.cont25:                                       ; preds = %if.end
  %35 = load { ptr, i64, i64 }, ptr %frames, align 8
  ret { ptr, i64, i64 } %35
}

define void @_V9Exception16report_unhandled(ptr %self) {
entry:
  %text = alloca { ptr, i64 }, align 8
  %call.result1 = alloca { ptr, i64 }, align 8
  %call.result = alloca { ptr, i64 }, align 8
  %field.message = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 0
  %0 = call { ptr, i64 } @"_V6String1+"(ptr @.strlit.6, ptr %field.message)
  store { ptr, i64 } %0, ptr %call.result, align 8
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  %1 = call { ptr, i64 } @"_V6String1+"(ptr %call.result, ptr @.strlit.8)
  store { ptr, i64 } %1, ptr %call.result1, align 8
  %exc.tag2 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending3 = icmp ne i32 %exc.tag2, -1
  br i1 %exc.pending3, label %exc.propagate4, label %exc.cont5

exc.propagate4:                                   ; preds = %exc.cont
  ret void

exc.cont5:                                        ; preds = %exc.cont
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %text, ptr align 8 %call.result1, i64 16, i1 false)
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %text, i32 0, i32 0
  %2 = load ptr, ptr %field.data, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %text, i32 0, i32 1
  %3 = load i64, ptr %field.size, align 8
  %4 = call i64 @write(i32 2, ptr %2, i64 %3)
  ret void
}

define { ptr, i64 } @_V9Exception16format_backtrace(ptr %self) {
entry:
  %call.result12 = alloca { ptr, i64 }, align 8
  %call.result7 = alloca { ptr, i64 }, align 8
  %call.result2 = alloca { ptr, i64 }, align 8
  %call.result = alloca { ptr, i64 }, align 8
  %i = alloca i64, align 8
  %result = alloca { ptr, i64 }, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %result, ptr align 8 @.strlit.10, i64 16, i1 false)
  store i64 0, ptr %i, align 8
  br label %while.cond

while.cond:                                       ; preds = %exc.cont16, %entry
  %0 = load i64, ptr %i, align 8
  %field.backtrace = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 1
  %field.size = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %field.backtrace, i32 0, i32 1
  %1 = load i64, ptr %field.size, align 8
  %2 = icmp ult i64 %0, %1
  br i1 %2, label %while.body, label %while.end

while.body:                                       ; preds = %while.cond
  %3 = call { ptr, i64 } @"_V6String1+"(ptr %result, ptr @.strlit.12)
  store { ptr, i64 } %3, ptr %call.result, align 8
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

while.end:                                        ; preds = %while.cond
  %4 = load { ptr, i64 }, ptr %result, align 8
  ret { ptr, i64 } %4

exc.propagate:                                    ; preds = %while.body
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %while.body
  %field.backtrace1 = getelementptr inbounds nuw { { ptr, i64 }, { ptr, i64, i64 }, i32 }, ptr %self, i32 0, i32 1
  %5 = load i64, ptr %i, align 8
  %6 = call { ptr, i64 } @"_V5Array2[]I6StringE"(ptr %field.backtrace1, i64 %5)
  store { ptr, i64 } %6, ptr %call.result2, align 8
  %exc.tag3 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending4 = icmp ne i32 %exc.tag3, -1
  br i1 %exc.pending4, label %exc.propagate5, label %exc.cont6

exc.propagate5:                                   ; preds = %exc.cont
  ret { ptr, i64 } zeroinitializer

exc.cont6:                                        ; preds = %exc.cont
  %7 = call { ptr, i64 } @"_V6String1+"(ptr %call.result, ptr %call.result2)
  store { ptr, i64 } %7, ptr %call.result7, align 8
  %exc.tag8 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending9 = icmp ne i32 %exc.tag8, -1
  br i1 %exc.pending9, label %exc.propagate10, label %exc.cont11

exc.propagate10:                                  ; preds = %exc.cont6
  ret { ptr, i64 } zeroinitializer

exc.cont11:                                       ; preds = %exc.cont6
  %8 = call { ptr, i64 } @"_V6String1+"(ptr %call.result7, ptr @.strlit.14)
  store { ptr, i64 } %8, ptr %call.result12, align 8
  %exc.tag13 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending14 = icmp ne i32 %exc.tag13, -1
  br i1 %exc.pending14, label %exc.propagate15, label %exc.cont16

exc.propagate15:                                  ; preds = %exc.cont11
  ret { ptr, i64 } zeroinitializer

exc.cont16:                                       ; preds = %exc.cont11
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %result, ptr align 8 %call.result12, i64 16, i1 false)
  %9 = load i64, ptr %i, align 8
  %10 = add i64 %9, 1
  store i64 %10, ptr %i, align 8
  br label %while.cond
}

define void @_V13ArgumentError10initialize(ptr %self, ptr %message) {
entry:
  call void @_V9Exception10initialize(ptr %self, ptr %message, i32 64)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  ret void
}

define void @_V10IndexError10initialize(ptr %self, ptr %message) {
entry:
  call void @_V9Exception10initialize(ptr %self, ptr %message, i32 64)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  ret void
}

define void @_V8KeyError10initialize(ptr %self, ptr %message) {
entry:
  call void @_V9Exception10initialize(ptr %self, ptr %message, i32 64)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  ret void
}

define void @_V17ZeroDivisionError10initialize(ptr %self, ptr %message) {
entry:
  call void @_V9Exception10initialize(ptr %self, ptr %message, i32 64)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  ret void
}

define void @_V9TypeError10initialize(ptr %self, ptr %message) {
entry:
  call void @_V9Exception10initialize(ptr %self, ptr %message, i32 64)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  ret void
}

define void @_V12RuntimeError10initialize(ptr %self, ptr %message) {
entry:
  call void @_V9Exception10initialize(ptr %self, ptr %message, i32 64)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  ret void
}

define i32 @"_V7Float323<=>"(float %self, float %other) {
entry:
  %other1 = alloca float, align 4
  store float %other, ptr %other1, align 4
  %0 = load float, ptr %other1, align 4
  %1 = fcmp olt float %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load float, ptr %other1, align 4
  %3 = fcmp ogt float %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define float @_V7Float323abs(float %self) {
entry:
  %0 = fcmp oge float %self, 0.000000e+00
  br i1 %0, label %ternary.then, label %ternary.else

ternary.then:                                     ; preds = %entry
  br label %ternary.end

ternary.else:                                     ; preds = %entry
  %1 = fneg float %self
  br label %ternary.end

ternary.end:                                      ; preds = %ternary.else, %ternary.then
  %ternary = phi float [ %self, %ternary.then ], [ %1, %ternary.else ]
  ret float %ternary
}

define i32 @"_V7Float643<=>"(double %self, double %other) {
entry:
  %other1 = alloca double, align 8
  store double %other, ptr %other1, align 8
  %0 = load double, ptr %other1, align 8
  %1 = fcmp olt double %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load double, ptr %other1, align 8
  %3 = fcmp ogt double %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i32 @"_V4Int83<=>"(i8 %self, i8 %other) {
entry:
  %other1 = alloca i8, align 1
  store i8 %other, ptr %other1, align 1
  %0 = load i8, ptr %other1, align 1
  %1 = icmp slt i8 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i8, ptr %other1, align 1
  %3 = icmp sgt i8 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V4Int84hash(i8 %self) {
entry:
  %0 = zext i8 %self to i64
  ret i64 %0
}

define i32 @"_V5Int163<=>"(i16 %self, i16 %other) {
entry:
  %other1 = alloca i16, align 2
  store i16 %other, ptr %other1, align 2
  %0 = load i16, ptr %other1, align 2
  %1 = icmp slt i16 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i16, ptr %other1, align 2
  %3 = icmp sgt i16 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V5Int164hash(i16 %self) {
entry:
  %0 = zext i16 %self to i64
  ret i64 %0
}

define i32 @"_V5Int323<=>"(i32 %self, i32 %other) {
entry:
  %other1 = alloca i32, align 4
  store i32 %other, ptr %other1, align 4
  %0 = load i32, ptr %other1, align 4
  %1 = icmp slt i32 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i32, ptr %other1, align 4
  %3 = icmp sgt i32 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V5Int324hash(i32 %self) {
entry:
  %h = alloca i32, align 4
  store i32 %self, ptr %h, align 4
  %0 = load i32, ptr %h, align 4
  %1 = load i32, ptr %h, align 4
  %2 = shl i32 %1, 13
  %3 = xor i32 %0, %2
  store i32 %3, ptr %h, align 4
  %4 = load i32, ptr %h, align 4
  %5 = load i32, ptr %h, align 4
  %6 = ashr i32 %5, 7
  %7 = xor i32 %4, %6
  store i32 %7, ptr %h, align 4
  %8 = load i32, ptr %h, align 4
  %9 = load i32, ptr %h, align 4
  %10 = shl i32 %9, 17
  %11 = xor i32 %8, %10
  %12 = zext i32 %11 to i64
  ret i64 %12
}

define i32 @"_V5Int643<=>"(i64 %self, i64 %other) {
entry:
  %other1 = alloca i64, align 8
  store i64 %other, ptr %other1, align 8
  %0 = load i64, ptr %other1, align 8
  %1 = icmp slt i64 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i64, ptr %other1, align 8
  %3 = icmp sgt i64 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V5Int644hash(i64 %self) {
entry:
  %h = alloca i64, align 8
  store i64 %self, ptr %h, align 8
  %0 = load i64, ptr %h, align 8
  %1 = load i64, ptr %h, align 8
  %2 = shl i64 %1, 13
  %3 = xor i64 %0, %2
  store i64 %3, ptr %h, align 8
  %4 = load i64, ptr %h, align 8
  %5 = load i64, ptr %h, align 8
  %6 = ashr i64 %5, 7
  %7 = xor i64 %4, %6
  store i64 %7, ptr %h, align 8
  %8 = load i64, ptr %h, align 8
  %9 = load i64, ptr %h, align 8
  %10 = shl i64 %9, 17
  %11 = xor i64 %8, %10
  ret i64 %11
}

define void @_V6String10initialize(ptr %self, ptr %data, i64 %size) {
entry:
  %size2 = alloca i64, align 8
  %data1 = alloca ptr, align 8
  store ptr %data, ptr %data1, align 8
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  store ptr %data, ptr %field.data, align 8
  store i64 %size, ptr %size2, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  store i64 %size, ptr %field.size, align 8
  ret void
}

define { ptr, i64 } @_V6String13from_c_string(ptr %ptr) {
entry:
  %new = alloca { ptr, i64 }, align 8
  %ptr1 = alloca ptr, align 8
  store ptr %ptr, ptr %ptr1, align 8
  %0 = load ptr, ptr %ptr1, align 8
  %1 = load ptr, ptr %ptr1, align 8
  %2 = call i64 @strlen(ptr %1)
  call void @_V6String10initialize(ptr %new, ptr %0, i64 %2)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %entry
  %3 = load { ptr, i64 }, ptr %new, align 8
  ret { ptr, i64 } %3
}

define i1 @"_V6String6empty?"(ptr %self) {
entry:
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %0 = load i64, ptr %field.size, align 8
  %1 = icmp eq i64 %0, 0
  ret i1 %1
}

define i8 @"_V6String2[]"(ptr %self, i64 %index) {
entry:
  %index1 = alloca i64, align 8
  store i64 %index, ptr %index1, align 8
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %0 = load ptr, ptr %field.data, align 8
  %1 = load i64, ptr %index1, align 8
  %2 = getelementptr i8, ptr %0, i64 %1
  %3 = load i8, ptr %2, align 1
  ret i8 %3
}

define void @"_V6String3[]="(ptr %self, i64 %index, i8 %value) {
entry:
  %value2 = alloca i8, align 1
  %index1 = alloca i64, align 8
  store i64 %index, ptr %index1, align 8
  store i8 %value, ptr %value2, align 1
  %0 = load i8, ptr %value2, align 1
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %1 = load ptr, ptr %field.data, align 8
  %2 = load i64, ptr %index1, align 8
  %3 = getelementptr i8, ptr %1, i64 %2
  store i8 %0, ptr %3, align 1
  ret void
}

define i32 @"_V6String3<=>"(ptr %self, ptr %other) {
entry:
  %cmp = alloca i32, align 4
  %min_size = alloca i64, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %0 = load i64, ptr %field.size, align 8
  %field.size1 = getelementptr inbounds nuw { ptr, i64 }, ptr %other, i32 0, i32 1
  %1 = load i64, ptr %field.size1, align 8
  %2 = icmp ult i64 %0, %1
  br i1 %2, label %ternary.then, label %ternary.else

ternary.then:                                     ; preds = %entry
  %field.size2 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %3 = load i64, ptr %field.size2, align 8
  br label %ternary.end

ternary.else:                                     ; preds = %entry
  %field.size3 = getelementptr inbounds nuw { ptr, i64 }, ptr %other, i32 0, i32 1
  %4 = load i64, ptr %field.size3, align 8
  br label %ternary.end

ternary.end:                                      ; preds = %ternary.else, %ternary.then
  %ternary = phi i64 [ %3, %ternary.then ], [ %4, %ternary.else ]
  store i64 %ternary, ptr %min_size, align 8
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %5 = load ptr, ptr %field.data, align 8
  %field.data4 = getelementptr inbounds nuw { ptr, i64 }, ptr %other, i32 0, i32 0
  %6 = load ptr, ptr %field.data4, align 8
  %7 = load i64, ptr %min_size, align 8
  %8 = call i32 @memcmp(ptr %5, ptr %6, i64 %7)
  store i32 %8, ptr %cmp, align 4
  %9 = load i32, ptr %cmp, align 4
  %10 = icmp eq i32 %9, 0
  %11 = call i1 @"_V4Bool1!"(i1 %10)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %ternary.end
  ret i32 0

exc.cont:                                         ; preds = %ternary.end
  br i1 %11, label %if.then, label %if.end

if.then:                                          ; preds = %exc.cont
  %12 = load i32, ptr %cmp, align 4
  ret i32 %12

if.end:                                           ; preds = %exc.cont
  %field.size5 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %13 = load i64, ptr %field.size5, align 8
  %field.size6 = getelementptr inbounds nuw { ptr, i64 }, ptr %other, i32 0, i32 1
  %14 = load i64, ptr %field.size6, align 8
  %15 = icmp ult i64 %13, %14
  br i1 %15, label %if.then7, label %if.else

if.then7:                                         ; preds = %if.end
  ret i32 -1

if.else:                                          ; preds = %if.end
  %field.size9 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %16 = load i64, ptr %field.size9, align 8
  %field.size10 = getelementptr inbounds nuw { ptr, i64 }, ptr %other, i32 0, i32 1
  %17 = load i64, ptr %field.size10, align 8
  %18 = icmp ugt i64 %16, %17
  br i1 %18, label %if.then11, label %if.else12

if.end8:                                          ; preds = %if.end13
  ret i32 0

if.then11:                                        ; preds = %if.else
  ret i32 1

if.else12:                                        ; preds = %if.else
  ret i32 0

if.end13:                                         ; No predecessors!
  br label %if.end8
}

define { ptr, i64 } @"_V6String1+"(ptr %self, ptr %other) {
entry:
  %new = alloca { ptr, i64 }, align 8
  %buf = alloca ptr, align 8
  %total = alloca i64, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %0 = load i64, ptr %field.size, align 8
  %field.size1 = getelementptr inbounds nuw { ptr, i64 }, ptr %other, i32 0, i32 1
  %1 = load i64, ptr %field.size1, align 8
  %2 = add i64 %0, %1
  store i64 %2, ptr %total, align 8
  %3 = load i64, ptr %total, align 8
  %4 = add i64 %3, 1
  %5 = call ptr @_V7Pointer6mallocI5UInt8E(i64 %4)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %entry
  store ptr %5, ptr %buf, align 8
  %6 = load ptr, ptr %buf, align 8
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %7 = load ptr, ptr %field.data, align 8
  %field.size2 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %8 = load i64, ptr %field.size2, align 8
  %9 = call ptr @memcpy(ptr %6, ptr %7, i64 %8)
  %10 = load ptr, ptr %buf, align 8
  %field.size3 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %11 = load i64, ptr %field.size3, align 8
  %12 = getelementptr i8, ptr %10, i64 %11
  %field.data4 = getelementptr inbounds nuw { ptr, i64 }, ptr %other, i32 0, i32 0
  %13 = load ptr, ptr %field.data4, align 8
  %field.size5 = getelementptr inbounds nuw { ptr, i64 }, ptr %other, i32 0, i32 1
  %14 = load i64, ptr %field.size5, align 8
  %15 = call ptr @memcpy(ptr %12, ptr %13, i64 %14)
  %16 = load ptr, ptr %buf, align 8
  %17 = load i64, ptr %total, align 8
  %18 = getelementptr i8, ptr %16, i64 %17
  store i8 0, ptr %18, align 1
  %19 = load ptr, ptr %buf, align 8
  %20 = load i64, ptr %total, align 8
  call void @_V6String10initialize(ptr %new, ptr %19, i64 %20)
  %exc.tag6 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending7 = icmp ne i32 %exc.tag6, -1
  br i1 %exc.pending7, label %exc.propagate8, label %exc.cont9

exc.propagate8:                                   ; preds = %exc.cont
  ret { ptr, i64 } zeroinitializer

exc.cont9:                                        ; preds = %exc.cont
  %21 = load { ptr, i64 }, ptr %new, align 8
  ret { ptr, i64 } %21
}

define i64 @_V6String4hash(ptr %self) {
entry:
  %i = alloca i64, align 8
  %h = alloca i32, align 4
  store i32 5381, ptr %h, align 4
  store i64 0, ptr %i, align 8
  br label %while.cond

while.cond:                                       ; preds = %while.body, %entry
  %0 = load i64, ptr %i, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %1 = load i64, ptr %field.size, align 8
  %2 = icmp ult i64 %0, %1
  br i1 %2, label %while.body, label %while.end

while.body:                                       ; preds = %while.cond
  %3 = load i32, ptr %h, align 4
  %4 = shl i32 %3, 5
  %5 = load i32, ptr %h, align 4
  %6 = add i32 %4, %5
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %7 = load ptr, ptr %field.data, align 8
  %8 = load i64, ptr %i, align 8
  %9 = getelementptr i8, ptr %7, i64 %8
  %10 = load i8, ptr %9, align 1
  %11 = zext i8 %10 to i32
  %12 = add i32 %6, %11
  %13 = and i32 %12, -1
  store i32 %13, ptr %h, align 4
  %14 = load i64, ptr %i, align 8
  %15 = add i64 %14, 1
  store i64 %15, ptr %i, align 8
  br label %while.cond

while.end:                                        ; preds = %while.cond
  %16 = load i32, ptr %h, align 4
  %17 = zext i32 %16 to i64
  ret i64 %17
}

define { ptr, i64 } @_V6String9to_string(ptr %self) {
entry:
  %0 = load { ptr, i64 }, ptr %self, align 8
  ret { ptr, i64 } %0
}

define { ptr, i64 } @_V6String4trim(ptr %self) {
entry:
  %new = alloca { ptr, i64 }, align 8
  %buf = alloca ptr, align 8
  %len = alloca i64, align 8
  %b12 = alloca i8, align 1
  %finish = alloca i64, align 8
  %b = alloca i8, align 1
  %start = alloca i64, align 8
  store i64 0, ptr %start, align 8
  br label %while.cond

while.cond:                                       ; preds = %if.end, %entry
  %0 = load i64, ptr %start, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %1 = load i64, ptr %field.size, align 8
  %2 = icmp ult i64 %0, %1
  br i1 %2, label %while.body, label %while.end

while.body:                                       ; preds = %while.cond
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %3 = load ptr, ptr %field.data, align 8
  %4 = load i64, ptr %start, align 8
  %5 = getelementptr i8, ptr %3, i64 %4
  %6 = load i8, ptr %5, align 1
  store i8 %6, ptr %b, align 1
  %7 = load i8, ptr %b, align 1
  %8 = icmp eq i8 %7, 32
  br i1 %8, label %or.end, label %or.rhs

while.end:                                        ; preds = %if.then, %while.cond
  %field.size7 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %9 = load i64, ptr %field.size7, align 8
  store i64 %9, ptr %finish, align 8
  br label %while.cond8

or.rhs:                                           ; preds = %while.body
  %10 = load i8, ptr %b, align 1
  %11 = icmp eq i8 %10, 9
  br label %or.end

or.end:                                           ; preds = %or.rhs, %while.body
  %logic = phi i1 [ true, %while.body ], [ %11, %or.rhs ]
  br i1 %logic, label %or.end2, label %or.rhs1

or.rhs1:                                          ; preds = %or.end
  %12 = load i8, ptr %b, align 1
  %13 = icmp eq i8 %12, 10
  br label %or.end2

or.end2:                                          ; preds = %or.rhs1, %or.end
  %logic3 = phi i1 [ true, %or.end ], [ %13, %or.rhs1 ]
  br i1 %logic3, label %or.end5, label %or.rhs4

or.rhs4:                                          ; preds = %or.end2
  %14 = load i8, ptr %b, align 1
  %15 = icmp eq i8 %14, 13
  br label %or.end5

or.end5:                                          ; preds = %or.rhs4, %or.end2
  %logic6 = phi i1 [ true, %or.end2 ], [ %15, %or.rhs4 ]
  %16 = call i1 @"_V4Bool1!"(i1 %logic6)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %or.end5
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %or.end5
  br i1 %16, label %if.then, label %if.end

if.then:                                          ; preds = %exc.cont
  br label %while.end

if.end:                                           ; preds = %exc.cont
  %17 = load i64, ptr %start, align 8
  %18 = add i64 %17, 1
  store i64 %18, ptr %start, align 8
  br label %while.cond

while.cond8:                                      ; preds = %if.end27, %while.end
  %19 = load i64, ptr %finish, align 8
  %20 = load i64, ptr %start, align 8
  %21 = icmp ugt i64 %19, %20
  br i1 %21, label %while.body9, label %while.end10

while.body9:                                      ; preds = %while.cond8
  %field.data11 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %22 = load ptr, ptr %field.data11, align 8
  %23 = load i64, ptr %finish, align 8
  %24 = sub i64 %23, 1
  %25 = getelementptr i8, ptr %22, i64 %24
  %26 = load i8, ptr %25, align 1
  store i8 %26, ptr %b12, align 1
  %27 = load i8, ptr %b12, align 1
  %28 = icmp eq i8 %27, 32
  br i1 %28, label %or.end14, label %or.rhs13

while.end10:                                      ; preds = %if.then26, %while.cond8
  %29 = load i64, ptr %finish, align 8
  %30 = load i64, ptr %start, align 8
  %31 = sub i64 %29, %30
  store i64 %31, ptr %len, align 8
  %32 = load i64, ptr %len, align 8
  %33 = add i64 %32, 1
  %34 = call ptr @_V7Pointer6mallocI5UInt8E(i64 %33)
  %exc.tag28 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending29 = icmp ne i32 %exc.tag28, -1
  br i1 %exc.pending29, label %exc.propagate30, label %exc.cont31

or.rhs13:                                         ; preds = %while.body9
  %35 = load i8, ptr %b12, align 1
  %36 = icmp eq i8 %35, 9
  br label %or.end14

or.end14:                                         ; preds = %or.rhs13, %while.body9
  %logic15 = phi i1 [ true, %while.body9 ], [ %36, %or.rhs13 ]
  br i1 %logic15, label %or.end17, label %or.rhs16

or.rhs16:                                         ; preds = %or.end14
  %37 = load i8, ptr %b12, align 1
  %38 = icmp eq i8 %37, 10
  br label %or.end17

or.end17:                                         ; preds = %or.rhs16, %or.end14
  %logic18 = phi i1 [ true, %or.end14 ], [ %38, %or.rhs16 ]
  br i1 %logic18, label %or.end20, label %or.rhs19

or.rhs19:                                         ; preds = %or.end17
  %39 = load i8, ptr %b12, align 1
  %40 = icmp eq i8 %39, 13
  br label %or.end20

or.end20:                                         ; preds = %or.rhs19, %or.end17
  %logic21 = phi i1 [ true, %or.end17 ], [ %40, %or.rhs19 ]
  %41 = call i1 @"_V4Bool1!"(i1 %logic21)
  %exc.tag22 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending23 = icmp ne i32 %exc.tag22, -1
  br i1 %exc.pending23, label %exc.propagate24, label %exc.cont25

exc.propagate24:                                  ; preds = %or.end20
  ret { ptr, i64 } zeroinitializer

exc.cont25:                                       ; preds = %or.end20
  br i1 %41, label %if.then26, label %if.end27

if.then26:                                        ; preds = %exc.cont25
  br label %while.end10

if.end27:                                         ; preds = %exc.cont25
  %42 = load i64, ptr %finish, align 8
  %43 = sub i64 %42, 1
  store i64 %43, ptr %finish, align 8
  br label %while.cond8

exc.propagate30:                                  ; preds = %while.end10
  ret { ptr, i64 } zeroinitializer

exc.cont31:                                       ; preds = %while.end10
  store ptr %34, ptr %buf, align 8
  %44 = load ptr, ptr %buf, align 8
  %field.data32 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %45 = load ptr, ptr %field.data32, align 8
  %46 = load i64, ptr %start, align 8
  %47 = getelementptr i8, ptr %45, i64 %46
  %48 = load i64, ptr %len, align 8
  %49 = call ptr @memcpy(ptr %44, ptr %47, i64 %48)
  %50 = load ptr, ptr %buf, align 8
  %51 = load i64, ptr %len, align 8
  %52 = getelementptr i8, ptr %50, i64 %51
  store i8 0, ptr %52, align 1
  %53 = load ptr, ptr %buf, align 8
  %54 = load i64, ptr %len, align 8
  call void @_V6String10initialize(ptr %new, ptr %53, i64 %54)
  %exc.tag33 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending34 = icmp ne i32 %exc.tag33, -1
  br i1 %exc.pending34, label %exc.propagate35, label %exc.cont36

exc.propagate35:                                  ; preds = %exc.cont31
  ret { ptr, i64 } zeroinitializer

exc.cont36:                                       ; preds = %exc.cont31
  %55 = load { ptr, i64 }, ptr %new, align 8
  ret { ptr, i64 } %55
}

define { ptr, i64 } @_V6String8downcase(ptr %self) {
entry:
  %new = alloca { ptr, i64 }, align 8
  %b = alloca i8, align 1
  %i = alloca i64, align 8
  %buf = alloca ptr, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %0 = load i64, ptr %field.size, align 8
  %1 = add i64 %0, 1
  %2 = call ptr @_V7Pointer6mallocI5UInt8E(i64 %1)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %entry
  store ptr %2, ptr %buf, align 8
  store i64 0, ptr %i, align 8
  br label %while.cond

while.cond:                                       ; preds = %if.end, %exc.cont
  %3 = load i64, ptr %i, align 8
  %field.size1 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %4 = load i64, ptr %field.size1, align 8
  %5 = icmp ult i64 %3, %4
  br i1 %5, label %while.body, label %while.end

while.body:                                       ; preds = %while.cond
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %6 = load ptr, ptr %field.data, align 8
  %7 = load i64, ptr %i, align 8
  %8 = getelementptr i8, ptr %6, i64 %7
  %9 = load i8, ptr %8, align 1
  store i8 %9, ptr %b, align 1
  %10 = load i8, ptr %b, align 1
  %11 = icmp uge i8 %10, 65
  br i1 %11, label %and.rhs, label %and.end

while.end:                                        ; preds = %while.cond
  %12 = load ptr, ptr %buf, align 8
  %field.size2 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %13 = load i64, ptr %field.size2, align 8
  %14 = getelementptr i8, ptr %12, i64 %13
  store i8 0, ptr %14, align 1
  %15 = load ptr, ptr %buf, align 8
  %field.size3 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %16 = load i64, ptr %field.size3, align 8
  call void @_V6String10initialize(ptr %new, ptr %15, i64 %16)
  %exc.tag4 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending5 = icmp ne i32 %exc.tag4, -1
  br i1 %exc.pending5, label %exc.propagate6, label %exc.cont7

and.rhs:                                          ; preds = %while.body
  %17 = load i8, ptr %b, align 1
  %18 = icmp ule i8 %17, 90
  br label %and.end

and.end:                                          ; preds = %and.rhs, %while.body
  %logic = phi i1 [ false, %while.body ], [ %18, %and.rhs ]
  br i1 %logic, label %if.then, label %if.else

if.then:                                          ; preds = %and.end
  %19 = load i8, ptr %b, align 1
  %20 = add i8 %19, 32
  %21 = load ptr, ptr %buf, align 8
  %22 = load i64, ptr %i, align 8
  %23 = getelementptr i8, ptr %21, i64 %22
  store i8 %20, ptr %23, align 1
  br label %if.end

if.else:                                          ; preds = %and.end
  %24 = load i8, ptr %b, align 1
  %25 = load ptr, ptr %buf, align 8
  %26 = load i64, ptr %i, align 8
  %27 = getelementptr i8, ptr %25, i64 %26
  store i8 %24, ptr %27, align 1
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  %28 = load i64, ptr %i, align 8
  %29 = add i64 %28, 1
  store i64 %29, ptr %i, align 8
  br label %while.cond

exc.propagate6:                                   ; preds = %while.end
  ret { ptr, i64 } zeroinitializer

exc.cont7:                                        ; preds = %while.end
  %30 = load { ptr, i64 }, ptr %new, align 8
  ret { ptr, i64 } %30
}

define { ptr, i64 } @_V6String6upcase(ptr %self) {
entry:
  %new = alloca { ptr, i64 }, align 8
  %b = alloca i8, align 1
  %i = alloca i64, align 8
  %buf = alloca ptr, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %0 = load i64, ptr %field.size, align 8
  %1 = add i64 %0, 1
  %2 = call ptr @_V7Pointer6mallocI5UInt8E(i64 %1)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %entry
  store ptr %2, ptr %buf, align 8
  store i64 0, ptr %i, align 8
  br label %while.cond

while.cond:                                       ; preds = %if.end, %exc.cont
  %3 = load i64, ptr %i, align 8
  %field.size1 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %4 = load i64, ptr %field.size1, align 8
  %5 = icmp ult i64 %3, %4
  br i1 %5, label %while.body, label %while.end

while.body:                                       ; preds = %while.cond
  %field.data = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 0
  %6 = load ptr, ptr %field.data, align 8
  %7 = load i64, ptr %i, align 8
  %8 = getelementptr i8, ptr %6, i64 %7
  %9 = load i8, ptr %8, align 1
  store i8 %9, ptr %b, align 1
  %10 = load i8, ptr %b, align 1
  %11 = icmp uge i8 %10, 97
  br i1 %11, label %and.rhs, label %and.end

while.end:                                        ; preds = %while.cond
  %12 = load ptr, ptr %buf, align 8
  %field.size2 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %13 = load i64, ptr %field.size2, align 8
  %14 = getelementptr i8, ptr %12, i64 %13
  store i8 0, ptr %14, align 1
  %15 = load ptr, ptr %buf, align 8
  %field.size3 = getelementptr inbounds nuw { ptr, i64 }, ptr %self, i32 0, i32 1
  %16 = load i64, ptr %field.size3, align 8
  call void @_V6String10initialize(ptr %new, ptr %15, i64 %16)
  %exc.tag4 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending5 = icmp ne i32 %exc.tag4, -1
  br i1 %exc.pending5, label %exc.propagate6, label %exc.cont7

and.rhs:                                          ; preds = %while.body
  %17 = load i8, ptr %b, align 1
  %18 = icmp ule i8 %17, 122
  br label %and.end

and.end:                                          ; preds = %and.rhs, %while.body
  %logic = phi i1 [ false, %while.body ], [ %18, %and.rhs ]
  br i1 %logic, label %if.then, label %if.else

if.then:                                          ; preds = %and.end
  %19 = load i8, ptr %b, align 1
  %20 = sub i8 %19, 32
  %21 = load ptr, ptr %buf, align 8
  %22 = load i64, ptr %i, align 8
  %23 = getelementptr i8, ptr %21, i64 %22
  store i8 %20, ptr %23, align 1
  br label %if.end

if.else:                                          ; preds = %and.end
  %24 = load i8, ptr %b, align 1
  %25 = load ptr, ptr %buf, align 8
  %26 = load i64, ptr %i, align 8
  %27 = getelementptr i8, ptr %25, i64 %26
  store i8 %24, ptr %27, align 1
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  %28 = load i64, ptr %i, align 8
  %29 = add i64 %28, 1
  store i64 %29, ptr %i, align 8
  br label %while.cond

exc.propagate6:                                   ; preds = %while.end
  ret { ptr, i64 } zeroinitializer

exc.cont7:                                        ; preds = %while.end
  %30 = load { ptr, i64 }, ptr %new, align 8
  ret { ptr, i64 } %30
}

define { ptr, i64 } @_V6String6prefix(ptr %self, ptr %pre) {
entry:
  %call.result = alloca { ptr, i64 }, align 8
  %0 = call { ptr, i64 } @"_V6String1+"(ptr %pre, ptr %self)
  store { ptr, i64 } %0, ptr %call.result, align 8
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret { ptr, i64 } zeroinitializer

exc.cont:                                         ; preds = %entry
  %1 = load { ptr, i64 }, ptr %call.result, align 8
  ret { ptr, i64 } %1
}

define void @_V6Symbol10initialize(ptr %self, i64 %id) {
entry:
  %id1 = alloca i64, align 8
  store i64 %id, ptr %id1, align 8
  %field.id = getelementptr inbounds nuw { i64 }, ptr %self, i32 0, i32 0
  store i64 %id, ptr %field.id, align 8
  ret void
}

define i32 @"_V6Symbol3<=>"(ptr %self, ptr %other) {
entry:
  %field.id = getelementptr inbounds nuw { i64 }, ptr %self, i32 0, i32 0
  %0 = load i64, ptr %field.id, align 8
  %field.id1 = getelementptr inbounds nuw { i64 }, ptr %other, i32 0, i32 0
  %1 = load i64, ptr %field.id1, align 8
  %2 = icmp ult i64 %0, %1
  br i1 %2, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %field.id2 = getelementptr inbounds nuw { i64 }, ptr %self, i32 0, i32 0
  %3 = load i64, ptr %field.id2, align 8
  %field.id3 = getelementptr inbounds nuw { i64 }, ptr %other, i32 0, i32 0
  %4 = load i64, ptr %field.id3, align 8
  %5 = icmp ugt i64 %3, %4
  br i1 %5, label %if.then4, label %if.else5

if.end:                                           ; preds = %if.end6
  ret i32 0

if.then4:                                         ; preds = %if.else
  ret i32 1

if.else5:                                         ; preds = %if.else
  ret i32 0

if.end6:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V6Symbol4hash(ptr %self) {
entry:
  %field.id = getelementptr inbounds nuw { i64 }, ptr %self, i32 0, i32 0
  %0 = load i64, ptr %field.id, align 8
  ret i64 %0
}

define i32 @"_V5UInt83<=>"(i8 %self, i8 %other) {
entry:
  %other1 = alloca i8, align 1
  store i8 %other, ptr %other1, align 1
  %0 = load i8, ptr %other1, align 1
  %1 = icmp ult i8 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i8, ptr %other1, align 1
  %3 = icmp ugt i8 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V5UInt84hash(i8 %self) {
entry:
  %0 = zext i8 %self to i64
  ret i64 %0
}

define i32 @"_V6UInt163<=>"(i16 %self, i16 %other) {
entry:
  %other1 = alloca i16, align 2
  store i16 %other, ptr %other1, align 2
  %0 = load i16, ptr %other1, align 2
  %1 = icmp ult i16 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i16, ptr %other1, align 2
  %3 = icmp ugt i16 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V6UInt164hash(i16 %self) {
entry:
  %0 = zext i16 %self to i64
  ret i64 %0
}

define i32 @"_V6UInt323<=>"(i32 %self, i32 %other) {
entry:
  %other1 = alloca i32, align 4
  store i32 %other, ptr %other1, align 4
  %0 = load i32, ptr %other1, align 4
  %1 = icmp ult i32 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i32, ptr %other1, align 4
  %3 = icmp ugt i32 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V6UInt324hash(i32 %self) {
entry:
  %h = alloca i32, align 4
  store i32 %self, ptr %h, align 4
  %0 = load i32, ptr %h, align 4
  %1 = load i32, ptr %h, align 4
  %2 = shl i32 %1, 13
  %3 = xor i32 %0, %2
  store i32 %3, ptr %h, align 4
  %4 = load i32, ptr %h, align 4
  %5 = load i32, ptr %h, align 4
  %6 = lshr i32 %5, 7
  %7 = xor i32 %4, %6
  store i32 %7, ptr %h, align 4
  %8 = load i32, ptr %h, align 4
  %9 = load i32, ptr %h, align 4
  %10 = shl i32 %9, 17
  %11 = xor i32 %8, %10
  %12 = zext i32 %11 to i64
  ret i64 %12
}

define i32 @"_V6UInt643<=>"(i64 %self, i64 %other) {
entry:
  %other1 = alloca i64, align 8
  store i64 %other, ptr %other1, align 8
  %0 = load i64, ptr %other1, align 8
  %1 = icmp ult i64 %self, %0
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  ret i32 -1

if.else:                                          ; preds = %entry
  %2 = load i64, ptr %other1, align 8
  %3 = icmp ugt i64 %self, %2
  br i1 %3, label %if.then2, label %if.else3

if.end:                                           ; preds = %if.end4
  ret i32 0

if.then2:                                         ; preds = %if.else
  ret i32 1

if.else3:                                         ; preds = %if.else
  ret i32 0

if.end4:                                          ; No predecessors!
  br label %if.end
}

define i64 @_V6UInt644hash(i64 %self) {
entry:
  %h = alloca i64, align 8
  store i64 %self, ptr %h, align 8
  %0 = load i64, ptr %h, align 8
  %1 = load i64, ptr %h, align 8
  %2 = shl i64 %1, 13
  %3 = xor i64 %0, %2
  store i64 %3, ptr %h, align 8
  %4 = load i64, ptr %h, align 8
  %5 = load i64, ptr %h, align 8
  %6 = lshr i64 %5, 7
  %7 = xor i64 %4, %6
  store i64 %7, ptr %h, align 8
  %8 = load i64, ptr %h, align 8
  %9 = load i64, ptr %h, align 8
  %10 = shl i64 %9, 17
  %11 = xor i64 %8, %10
  ret i64 %11
}

declare i32 @backtrace(ptr, i32)

declare ptr @backtrace_symbols(ptr, i32)

declare i64 @write(i32, ptr, i64)

declare ptr @malloc(i64)

declare void @free(ptr)

declare ptr @memcpy(ptr, ptr, i64)

declare i32 @memcmp(ptr, ptr, i64)

declare i64 @strlen(ptr)

declare void @exit(i32)

define i1 @_V06is_nil(ptr %p) {
entry:
  %p1 = alloca ptr, align 8
  store ptr %p, ptr %p1, align 8
  %0 = load ptr, ptr %p1, align 8
  %1 = icmp eq ptr %0, null
  ret i1 %1
}

define i32 @_V04main() {
entry:
  %0 = call i1 @_V06is_nil(ptr null)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret i32 0

exc.cont:                                         ; preds = %entry
  br i1 %0, label %ternary.then, label %ternary.else

ternary.then:                                     ; preds = %exc.cont
  br label %ternary.end

ternary.else:                                     ; preds = %exc.cont
  br label %ternary.end

ternary.end:                                      ; preds = %ternary.else, %ternary.then
  %ternary = phi i32 [ 9, %ternary.then ], [ 0, %ternary.else ]
  ret i32 %ternary
}

define void @_V_init_0() {
entry:
  ret void
}

define void @_V_init_1() {
entry:
  ret void
}

define void @_V_init_2() {
entry:
  ret void
}

define void @_V_init_3() {
entry:
  ret void
}

define void @_V_init_4() {
entry:
  ret void
}

define void @_V_init_5() {
entry:
  ret void
}

define void @_V_init_6() {
entry:
  ret void
}

define void @_V_init_7() {
entry:
  ret void
}

define void @_V_init_8() {
entry:
  ret void
}

define void @_V_init_9() {
entry:
  ret void
}

define void @_V_init_10() {
entry:
  ret void
}

define void @_V_init_11() {
entry:
  ret void
}

define void @_V_init_12() {
entry:
  ret void
}

define void @_V_init_13() {
entry:
  ret void
}

define void @_V_init_14() {
entry:
  ret void
}

define void @_V_init_15() {
entry:
  ret void
}

define void @_V_init_16() {
entry:
  ret void
}

define void @_V_init_17() {
entry:
  ret void
}

define void @_V_init_18() {
entry:
  ret void
}

define void @_V_init_19() {
entry:
  %0 = call i32 @_V04main()
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  call void @exit(i32 %0)
  ret void
}

define linkonce_odr ptr @_V7Pointer6mallocI5UInt8E(i64 %count) {
entry:
  %count1 = alloca i64, align 8
  store i64 %count, ptr %count1, align 8
  %0 = load i64, ptr %count1, align 8
  %1 = mul i64 %0, 1
  %2 = call ptr @malloc(i64 %1)
  ret ptr %2
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg) #0

define linkonce_odr ptr @_V7Pointer6mallocI7PointerIEE(i64 %count) {
entry:
  %count1 = alloca i64, align 8
  store i64 %count, ptr %count1, align 8
  %0 = load i64, ptr %count1, align 8
  %1 = mul i64 %0, 8
  %2 = call ptr @malloc(i64 %1)
  ret ptr %2
}

define linkonce_odr void @_V5Array10initializeI6StringE(ptr %self, i64 %initial_capacity) {
entry:
  %initial_capacity1 = alloca i64, align 8
  store i64 %initial_capacity, ptr %initial_capacity1, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 1
  store i64 0, ptr %field.size, align 8
  %0 = load i64, ptr %initial_capacity1, align 8
  %field.capacity = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 2
  store i64 %0, ptr %field.capacity, align 8
  %field.capacity2 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 2
  %1 = load i64, ptr %field.capacity2, align 8
  %2 = call ptr @_V7Pointer6mallocI6StringE(i64 %1)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %entry
  ret void

exc.cont:                                         ; preds = %entry
  %field.buffer = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 0
  store ptr %2, ptr %field.buffer, align 8
  ret void
}

define linkonce_odr void @_V5Array4pushI6StringE(ptr %self, ptr %value) {
entry:
  %i = alloca i64, align 8
  %new_buf = alloca ptr, align 8
  %new_cap = alloca i64, align 8
  %field.size = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 1
  %0 = load i64, ptr %field.size, align 8
  %field.capacity = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 2
  %1 = load i64, ptr %field.capacity, align 8
  %2 = icmp uge i64 %0, %1
  br i1 %2, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  %field.capacity1 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 2
  %3 = load i64, ptr %field.capacity1, align 8
  %4 = icmp eq i64 %3, 0
  br i1 %4, label %ternary.then, label %ternary.else

if.end:                                           ; preds = %exc.cont8, %entry
  %field.buffer11 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 0
  %5 = load ptr, ptr %field.buffer11, align 8
  %field.size12 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 1
  %6 = load i64, ptr %field.size12, align 8
  %7 = getelementptr { ptr, i64 }, ptr %5, i64 %6
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %7, ptr align 8 %value, i64 16, i1 false)
  %field.size13 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 1
  %8 = load i64, ptr %field.size13, align 8
  %9 = add i64 %8, 1
  %field.size14 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 1
  store i64 %9, ptr %field.size14, align 8
  ret void

ternary.then:                                     ; preds = %if.then
  br label %ternary.end

ternary.else:                                     ; preds = %if.then
  %field.capacity2 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 2
  %10 = load i64, ptr %field.capacity2, align 8
  %11 = mul i64 %10, 2
  br label %ternary.end

ternary.end:                                      ; preds = %ternary.else, %ternary.then
  %ternary = phi i64 [ 8, %ternary.then ], [ %11, %ternary.else ]
  store i64 %ternary, ptr %new_cap, align 8
  %12 = load i64, ptr %new_cap, align 8
  %13 = call ptr @_V7Pointer6mallocI6StringE(i64 %12)
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.propagate, label %exc.cont

exc.propagate:                                    ; preds = %ternary.end
  ret void

exc.cont:                                         ; preds = %ternary.end
  store ptr %13, ptr %new_buf, align 8
  store i64 0, ptr %i, align 8
  br label %while.cond

while.cond:                                       ; preds = %while.body, %exc.cont
  %14 = load i64, ptr %i, align 8
  %field.size3 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 1
  %15 = load i64, ptr %field.size3, align 8
  %16 = icmp ult i64 %14, %15
  br i1 %16, label %while.body, label %while.end

while.body:                                       ; preds = %while.cond
  %field.buffer = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 0
  %17 = load ptr, ptr %field.buffer, align 8
  %18 = load i64, ptr %i, align 8
  %19 = getelementptr { ptr, i64 }, ptr %17, i64 %18
  %20 = load ptr, ptr %new_buf, align 8
  %21 = load i64, ptr %i, align 8
  %22 = getelementptr { ptr, i64 }, ptr %20, i64 %21
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %22, ptr align 8 %19, i64 16, i1 false)
  %23 = load i64, ptr %i, align 8
  %24 = add i64 %23, 1
  store i64 %24, ptr %i, align 8
  br label %while.cond

while.end:                                        ; preds = %while.cond
  %field.buffer4 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 0
  %25 = load ptr, ptr %field.buffer4, align 8
  call void @_V7Pointer4freeI6StringE(ptr %25)
  %exc.tag5 = load i32, ptr @volt.exc.tag, align 4
  %exc.pending6 = icmp ne i32 %exc.tag5, -1
  br i1 %exc.pending6, label %exc.propagate7, label %exc.cont8

exc.propagate7:                                   ; preds = %while.end
  ret void

exc.cont8:                                        ; preds = %while.end
  %26 = load ptr, ptr %new_buf, align 8
  %field.buffer9 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 0
  store ptr %26, ptr %field.buffer9, align 8
  %27 = load i64, ptr %new_cap, align 8
  %field.capacity10 = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 2
  store i64 %27, ptr %field.capacity10, align 8
  br label %if.end
}

define linkonce_odr void @_V7Pointer4freeI7PointerI5UInt8EE(ptr %self) {
entry:
  call void @free(ptr %self)
  ret void
}

define linkonce_odr void @_V7Pointer4freeI7PointerIEE(ptr %self) {
entry:
  call void @free(ptr %self)
  ret void
}

define linkonce_odr { ptr, i64 } @"_V5Array2[]I6StringE"(ptr %self, i64 %index) {
entry:
  %index1 = alloca i64, align 8
  store i64 %index, ptr %index1, align 8
  %field.buffer = getelementptr inbounds nuw { ptr, i64, i64 }, ptr %self, i32 0, i32 0
  %0 = load ptr, ptr %field.buffer, align 8
  %1 = load i64, ptr %index1, align 8
  %2 = getelementptr { ptr, i64 }, ptr %0, i64 %1
  %3 = load { ptr, i64 }, ptr %2, align 8
  ret { ptr, i64 } %3
}

define linkonce_odr ptr @_V7Pointer6mallocI6StringE(i64 %count) {
entry:
  %count1 = alloca i64, align 8
  store i64 %count, ptr %count1, align 8
  %0 = load i64, ptr %count1, align 8
  %1 = mul i64 %0, 16
  %2 = call ptr @malloc(i64 %1)
  ret ptr %2
}

define linkonce_odr void @_V7Pointer4freeI6StringE(ptr %self) {
entry:
  call void @free(ptr %self)
  ret void
}

define i32 @main(i32 %0, ptr %1) {
entry:
  call void @_V_init_0()
  call void @_V_init_1()
  call void @_V_init_2()
  call void @_V_init_3()
  call void @_V_init_4()
  call void @_V_init_5()
  call void @_V_init_6()
  call void @_V_init_7()
  call void @_V_init_8()
  call void @_V_init_9()
  call void @_V_init_10()
  call void @_V_init_11()
  call void @_V_init_12()
  call void @_V_init_13()
  call void @_V_init_14()
  call void @_V_init_15()
  call void @_V_init_16()
  call void @_V_init_17()
  call void @_V_init_18()
  call void @_V_init_19()
  %exc.tag = load i32, ptr @volt.exc.tag, align 4
  %exc.pending = icmp ne i32 %exc.tag, -1
  br i1 %exc.pending, label %exc.unhandled, label %exit.clean

exc.unhandled:                                    ; preds = %entry
  call void @_V9Exception16report_unhandled(ptr @volt.exc.storage)
  ret i32 1

exit.clean:                                       ; preds = %entry
  ret i32 0
}

attributes #0 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }
