module Volt::Frontend


  enum Prec : Int32
    None       =  0
    Modifier   =  1   # postfix if / unless / while / until
    Assignment =  2   # =  += -= etc.
    Ternary    =  3   # ? :
    Or         =  4   # or  ||
    And        =  5   # and  &&
    Equality   =  6   # ==  !=  ===  =~  !~
    Comparison =  7   # <  >  <=  >=  <=>
    Range      =  8   # ..  ...
    Pipe       =  9   # |>
    BitOr      = 10   # |  ^
    BitAnd     = 11   # &
    Shift      = 12   # <<  >>
    Term       = 13   # +  -  &+  &-
    Factor     = 14   # *  /  //  %  &*
    Power      = 15   # **  &**
    Unary      = 16   # !  not  -(prefix)  ~  +(prefix)
    Call       = 17   # ()  []  .  ?.
    Primary    = 18
  end


end
