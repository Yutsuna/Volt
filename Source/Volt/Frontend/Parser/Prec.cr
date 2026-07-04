module Volt::Frontend


  enum Prec : Int32
    None       =  0
    Modifier   =  1   # postfix if / unless / while / until
    Assignment =  2   # =  += -= etc.
    Or         =  3   # or  ||
    And        =  4   # and  &&
    Equality   =  5   # ==  !=  ===  =~  !~
    Comparison =  6   # <  >  <=  >=  <=>
    Range      =  7   # ..  ...
    Pipe       =  8   # |>
    BitOr      =  9   # |  ^
    BitAnd     = 10   # &
    Shift      = 11   # <<  >>
    Term       = 12   # +  -  &+  &-
    Factor     = 13   # *  /  //  %  &*
    Power      = 14   # **  &**
    Unary      = 15   # !  not  -(prefix)  ~  +(prefix)
    Call       = 16   # ()  []  .  ?.
    Primary    = 17
  end


end
