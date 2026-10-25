(* Tank level control

   Keeps the level between start_level and stop_level while auto_mode is on.
   In manual mode the pump follows whatever an operator writes to pump_run.
   The high alarm always stops the pump.

   The inputs come from the field: the level from a level transmitter and
   high_alarm from a level switch, written over Modbus by a remote I/O module
   or a link. To test without a tank, let a simulator such as Factory I/O, or
   any Modbus client, write level and high_alarm. *)
PROGRAM main
VAR_EXTERNAL
    pump_run : BOOL;
    drain_open : BOOL;
    auto_mode : BOOL;
    high_alarm : BOOL;
    start_level : UINT;
    stop_level : UINT;
    pump_starts : UINT;
    level : REAL;
END_VAR
VAR
    pump : BOOL;
    pump_started : R_TRIG;
END_VAR
    (* Operator defaults on a fresh PLC; later starts keep what operators set *)
    IF FIRST_CYCLE() AND stop_level = 0 THEN
        start_level := 30;
        stop_level := 80;
        auto_mode := TRUE;
        drain_open := TRUE;
    END_IF;

    pump := pump_run;
    IF auto_mode THEN
        IF level <= start_level THEN
            pump := TRUE;
        END_IF;
        IF level >= stop_level THEN
            pump := FALSE;
        END_IF;
    END_IF;
    IF high_alarm THEN
        pump := FALSE;  (* interlock *)
    END_IF;
    pump_run := pump;

    pump_started(CLK := pump);
    IF pump_started.Q AND pump_starts < 65535 THEN
        pump_starts := pump_starts + 1;
    END_IF;
END_PROGRAM
