module Volt::CLI


  class Terminal

    @original : LibC::Termios = LibC::Termios.new
    @active : Bool = false
    STDIN_FD = 0

    # Linux c_cc indices, missing from Crystal's LibC bindings.
    VTIME = 5
    VMIN  = 6

    def enable_raw_mode : Nil
      return if @active

      if LibC.tcgetattr(STDIN_FD, pointerof(@original)) == 0
        @active = true

        raw = @original
        raw.c_lflag &= ~(LibC::ECHO | LibC::ICANON | LibC::ISIG)
        raw.c_cc[VMIN]  = 1_u8
        raw.c_cc[VTIME] = 0_u8

        LibC.tcsetattr(STDIN_FD, 0, pointerof(raw))
      end
    end

    def restore : Nil
      return unless @active
      @active = false if LibC.tcsetattr(STDIN_FD, 0, pointerof(@original)) == 0
    end
  end


end
