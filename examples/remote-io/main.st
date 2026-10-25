(* Remote I/O: watches the level of another PLC over Modbus TCP.

   This PLC is the Modbus TCP client. Its link "tank" (Communication page, or "links" in the
   configuration) reads holding registers 3-4 (float32) of the PLC named tank, which
   runs the tank example, into remote_level every 200 ms, and keeps link_ok TRUE
   while that device answers. Point the link at another device to read something
   else; the program only uses tags. *)
PROGRAM main
VAR_EXTERNAL CONSTANT
    link_ok : BOOL;       (* written by the link *)
    remote_level : REAL;  (* written by the link *)
END_VAR
VAR_EXTERNAL
    alarm_level : UINT;
    level_alarm : BOOL;
    link_alarm : BOOL;
END_VAR
    IF FIRST_CYCLE() AND alarm_level = 0 THEN
        alarm_level := 85;
    END_IF;

    (* A level from a link that is down is stale: decide nothing on it *)
    link_alarm := NOT link_ok;
    level_alarm := link_ok AND remote_level > alarm_level;
END_PROGRAM
