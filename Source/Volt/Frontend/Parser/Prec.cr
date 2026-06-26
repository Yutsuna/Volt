module Volt::Frontend


  enum Prec : Int32
    None       =  0
    Assignment =  1   # =  += -= etc.
    Or         =  2   # or  ||
    And        =  3   # and  &&
    Equality   =  4   # ==  !=  ===  =~  !~
    Comparison =  5   # <  >  <=  >=  <=>
    Range      =  6   # ..  ...
    Pipe       =  7   # |>
    BitOr      =  8   # |  ^
    BitAnd     =  9   # &
    Shift      = 10   # <<  >>
    Term       = 11   # +  -  &+  &-
    Factor     = 12   # *  /  //  %  &*
    Power      = 13   # **  &**
    Unary      = 14   # !  not  -(prefix)  ~  +(prefix)
    Call       = 15   # ()  []  .  ?.
    Primary    = 16
  end


end
