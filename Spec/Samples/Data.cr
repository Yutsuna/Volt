require "../SpecHelper"


module Volt::Spec


  SAMPLES_01_DIR = File.expand_path("../../Samples/Tests/Functional/01/01.*.vl", __DIR__)
  SAMPLES_02_DIR = File.expand_path("../../Samples/Tests/Functional/02/*.vl", __DIR__)


  class ExpectedResults

    include MStreamable

    EXPECTED_DATA = {
      "01.0.return_exit" => {exit_code: 84, stderr: "", stdout: ""},
      #------------------------------------------------------------------------------------
      "01.a.HelloWorld.vl"  =>  {exit_code: 0,  stderr: "", stdout: "Hello, Volt!\n"},
      #------------------------------------------------------------------------------------
      "01.b.Arithmetic.vl"  =>  {exit_code: 0,  stderr: "", stdout: "PASS\n"*44},
      #------------------------------------------------------------------------------------
      "01.c.Macro.vl"       =>  {exit_code: 0,  stderr: "", stdout: "[INFO] #{File.dirname(SAMPLES_01_DIR)}/01.c.Macro.vl (line 6) : expression is: foo\n[INFO] #{File.dirname(SAMPLES_01_DIR)}/01.c.Macro.vl (line 8) : expression is: bar\n"},
      #------------------------------------------------------------------------------------
      "01.d.MacroControl.vl" => {exit_code: 0,  stderr: "", stdout: "LOW\nHIGH\na fruit\na vegetable\nunknown\n"},
      #------------------------------------------------------------------------------------
      "01.e.MacroCodegen.vl" => {exit_code: 0,  stderr: "", stdout: "world\nvolt\nsection\ndone\n"},
      #------------------------------------------------------------------------------------
      "01.f.StructNoMethods.vl" => {exit_code: 0,  stderr: "", stdout: "PASS\n"},
      #------------------------------------------------------------------------------------
      "01.g.StructWithMethods.vl" => {exit_code: 0,  stderr: "", stdout: "PASS\n"},
      #------------------------------------------------------------------------------------
      "01.h.ClassRaii.vl" => {exit_code: 0,  stderr: "", stdout: "ENTERING SCOPE\nCONNECTED DB 101\nINIT SESSION_MANAGER\nLEAVING SCOPE\nFINALIZE SESSION_MANAGER\nDISCONNECTED DB 101\nTEST COMPLETE\n"},
      #------------------------------------------------------------------------------------
      "01.i.ClassInheritance.vl" => {exit_code: 0,  stderr: "", stdout: "WRITE TO USB [FlashDrive] (16384MB): kernel_backup.bin\nPRINT TO NETWORK [OfficeJet]: report.pdf\n"},
      #------------------------------------------------------------------------------------
      "01.j.ClassMixin.vl" => {exit_code: 0,  stderr: "", stdout: "[LOG] Verifying credentials for admin\n[LOG] Executing 'rm -rf /tmp' under session SESS_999\n[LOG] Verifying credentials for guest\n[LOG] DENIED access to guest\n"},
      #------------------------------------------------------------------------------------
      "01.k.ClassRAIIEarlyExit.vl" => {exit_code: 0, stderr: "runtime error: ConstructorFailureException\n", stdout: "Early Return\nLOCK acquired: MainTx\nLOCK acquired: ErrorLog\nLOCK released: MainTx\nLOCK released: ErrorLog\nConstruction Crash\nLOCK acquired: NestedLock\nSimulating crash inside constructor...\n"},
      #------------------------------------------------------------------------------------
      "01.l.ClassMixinAbstract.vl" => {exit_code: 0,  stderr: "", stdout: "Book: 'Design Patterns' by Gang of Four\nElectronic: VoltBook Pro (24 months warranty)\nBook Tax: 11.0\nLaptop Tax: 299.998\n"},
      #------------------------------------------------------------------------------------
      "01.m.RpgPolymorphism.vl" => {exit_code: 0, stderr: "", stdout: "Paladin Arthas HP: 120/120\nAction: Holy Shield active (Shield: 25)\nRanger Sylvanas HP: 90/90\nAction: Double Shot! (Arrows remaining: 1)\nRanger Sylvanas HP: 90/90\nAction: Double Shot! (Arrows remaining: 0)\nRanger Sylvanas HP: 90/90\nAction: Out of arrows!\nPaladin Arthas HP: 150/150\nAction: Holy Shield active (Shield: 35)\nRanger Sylvanas HP: 110/110\nAction: Out of arrows!\n"},
      #------------------------------------------------------------------------------------
      "01.n.SmartCityOOP.vl" => {exit_code: 0, stderr: "", stdout: "System 'Sensor-Alpha' initializing hardware components...\nSystem 'Panel-Solaris' initializing hardware components...\n[IoT-Log] Connecting to City_Grid_5G...\n[IoT-Log] Connecting to City_Grid_5G...\n--- Testing Device ---\nDiagnostic : WaterSensor check: Flow rate is stable at 12.4 L/s.\nEnergy Impact : 0.05 W\n--- Testing Device ---\nDiagnostic : SolarPanel check: Grid synchronization optimal. Location: [48.8566, 2.3522]\nEnergy Impact : -348.5 W\nSensor status: ONLINE\nPanel status: ONLINE\n"},
      #------------------------------------------------------------------------------------
      "01.o.ModuleDatabase.vl" => {exit_code: 0, stderr: "", stdout: "[DN-INTERNAL] Connection established: Current 1\nAttempt 1: true\n[DN-INTERNAL] Connection established: Current 2\nAttempt 2: true\nActive connections: 2\n"},
      #------------------------------------------------------------------------------------
      "01.p.ModuleMaths.vl" => {exit_code: 0, stderr: "", stdout: "PASS\n"},
      #------------------------------------------------------------------------------------
      "01.q.Prec.vl" => {exit_code: 0, stderr: "", stdout: "PASS\n"*12},
      #------------------------------------------------------------------------------------
      "01.r.ClassSuper.vl" => {exit_code: 0, stderr: "", stdout: "BUILD Vehicle Golf\nBUILD Car Golf\nVehicle Golf @ 180km/h\nCar with 4 wheels\nstd: Golf [car]\nBUILD Vehicle Falcon\nBUILD Car Falcon\nBUILD RaceCar Falcon\nVehicle Falcon @ 300km/h\nCar with 4 wheels\nRACE: Falcon [car]!\nBUILD Vehicle Zoe\nBUILD Car Zoe\nVehicle Zoe @ 140km/h\nCar with 4 wheels\nEV mode\n"},
      #------------------------------------------------------------------------------------
      "01.s.Polymorphism.vl" => {exit_code: 0, stderr: "", stdout: ""},
      #------------------------------------------------------------------------------------
      "01.t.RaiiDtor.vl" => {exit_code: 0, stderr: "", stdout: "__CTOR__ global\n__CTOR__ func\n__DTOR__ func\n__DTOR__ global\n"},
      #------------------------------------------------------------------------------------
      "01.u.RawPointers.vl" => {exit_code: 0, stderr: "", stdout: "PASS LOCAL ADDR\nPASS NIL EQ\nPASS NONNIL NE\nPASS CLASS IVAR ADDR\nPASS STRUCT FIELD ADDR\nPASS MALLOC WRITE 0\nPASS MALLOC WRITE 1\n"},
      #------------------------------------------------------------------------------------
      "01.v.Typeof.vl" => {exit_code: 0, stderr: "", stdout: "PASS\n"*7},
      #------------------------------------------------------------------------------------
      "02.vl" => {exit_code: 0, stderr: "", stdout: ""},
      #------------------------------------------------------------------------------------
      "02.a.String.vl" => {exit_code: 0, stderr: "", stdout: "CTOR\nDTOR\n"},
      #------------------------------------------------------------------------------------
      "02.b.Hash.vl" => {exit_code: 0, stderr: "", stdout: "PASS GET A\nPASS GET B\nPASS UPDATE A\nPASS HAS B\nPASS NOT HAS C\n"},
      #------------------------------------------------------------------------------------
      "02.c.PrimitiveReopen.vl" => {exit_code: 0, stderr: "", stdout: "double: 42\nliteral receiver: 10\nnegative: 0\nchained: 84\nyes\nno\n"},
      #------------------------------------------------------------------------------------
      "02.d.Pipeline.vl" => {exit_code: 0, stderr: "", stdout: "SUCCESS : [15]\n"},
      #------------------------------------------------------------------------------------
      "02.e.PipelineEdgeCases.vl" => {exit_code: 0, stderr: "", stdout: "PASS\n"*6},
      #------------------------------------------------------------------------------------
      "02.f.templates.vl" => {exit_code: 0, stderr: "", stdout: "PASS\n"*6},
      #------------------------------------------------------------------------------------
      "02.g.forall.vl" => {exit_code: 0, stderr: "", stdout: "PASS\n"*6},
      #------------------------------------------------------------------------------------
      "02.h.Memory.vl" => {exit_code: 0, stderr: "", stdout: "PASS\n"*101},
      #------------------------------------------------------------------------------------
    }

    def initialize( file_path : String )
      data        = EXPECTED_DATA[ file_path.split("/").last ]
      @exit_code  = data[:exit_code]
      @stderr     = data[:stderr]
      @stdout     = data[:stdout]
    end

    def self.run( file_path : String )
      new file_path
    end

  end


end
