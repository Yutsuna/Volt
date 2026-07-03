module Volt::CLI


  class Terminal

    @original : LibC::Termios = LibC::Termios.new
    @active : Bool = false
    STDIN_FD = 0

    def enable_raw_mode : Nil
      return if @active

      if LibC.tcgetattr(STDIN_FD, pointerof(@original)) == 0
        @active = true

        raw = @original
        raw.c_lflag &= ~(LibC::ECHO | LibC::ICANON | LibC::ISIG)

        LibC.tcsetattr(STDIN_FD, 0, pointerof(raw))
      end
    end

    def restore : Nil
      return unless @active
      @active = false if LibC.tcsetattr(STDIN_FD, 0, pointerof(@original)) == 0
    end
  end


end
