module Volt::CLI


  enum KeyEvent
    ArrowUp
    ArrowDown
    ArrowLeft
    ArrowRight
    CtrlLeft
    CtrlRight
    Backspace
    Delete
    CtrlBackspace   # delete the word left of the cursor
    CtrlDelete      # delete the word right of the cursor
    Home
    End
    Tab
    ShiftTab
    Escape
    CtrlA
    CtrlC
    CtrlD
    CtrlE
    Enter
    Char
    Text
    Ignored
  end


end
