(* Temperature control with a PID function block

   Holds temperature at setpoint with a heater driven 0-100 %. auto_mode off: the heater
   follows manual_heater, and the controller tracks it, so switching back to automatic
   does not make the heater jump (bumpless transfer).

   The temperature comes from the field: a transmitter written over Modbus by a remote
   I/O module or a link. heater goes to a thyristor unit or an SSR with a 0-100 % input;
   at STOP and on a fault it gets its substitute value, 0 %.

   PidController is the standard positional PID: the derivative acts on the measurement
   (a setpoint step gives no kick) through a filter of Td / 10, and the integral stands
   still while the output is at a limit (anti-windup). It is called every cycle with
   dt = the task's cycle time, as PID_Compact in a cyclic OB: change both together.

   Tuning: kp in % per K, ti and td in s (ti 0: no integral). The defaults suit a
   slow heater (a time constant of minutes); tune them for the plant. *)
FUNCTION_BLOCK PidController
VAR_INPUT
    sp : REAL;              (* setpoint *)
    pv : REAL;              (* measured value *)
    kp : REAL;              (* gain, output per unit of error *)
    ti : REAL;              (* integral time, s; 0: none *)
    td : REAL;              (* derivative time, s; 0: none *)
    dt : REAL;              (* time between calls, s *)
    auto : BOOL;            (* FALSE: the output is manual_y *)
    manual_y : REAL;
    y_min : REAL;
    y_max : REAL;
END_VAR
VAR_OUTPUT
    y : REAL;               (* output, within y_min..y_max *)
    e : REAL;               (* error sp - pv *)
END_VAR
VAR
    p, i, d : REAL;         (* the three parts of y *)
    pv_last : REAL;
    started : BOOL;
END_VAR
VAR_TEMP
    tf, di : REAL;
END_VAR
    IF NOT started THEN
        pv_last := pv;      (* no derivative kick in the first call *)
        started := TRUE;
    END_IF;
    e := sp - pv;
    p := kp * e;
    IF td > 0.0 AND dt > 0.0 THEN
        tf := td / 10.0;
        d := (tf * d - kp * td * (pv - pv_last)) / (tf + dt);
    ELSE
        d := 0.0;
    END_IF;
    pv_last := pv;
    IF NOT auto THEN
        y := LIMIT(y_min, manual_y, y_max);
        i := y - p - d;     (* tracks the manual output: no jump when auto comes back *)
    ELSE
        IF ti > 0.0 THEN
            di := kp * dt / ti * e;
            (* Anti-windup, as PID_Compact: no integration while it would push the output further past a limit *)
            IF NOT ((p + i + d >= y_max AND di > 0.0) OR (p + i + d <= y_min AND di < 0.0)) THEN
                i := i + di;
            END_IF;
        END_IF;
        y := LIMIT(y_min, p + i + d, y_max);
    END_IF;
END_FUNCTION_BLOCK

PROGRAM main
VAR_EXTERNAL
    temperature : REAL;
    setpoint : REAL;
    heater : REAL;
    manual_heater : REAL;
    kp : REAL;
    ti : REAL;
    td : REAL;
    auto_mode : BOOL;
    at_setpoint : BOOL;
END_VAR
VAR
    pid : PidController;
END_VAR
    (* Operator defaults on a fresh PLC; later starts keep what operators set *)
    IF FIRST_CYCLE() AND kp = 0.0 THEN
        setpoint := 60.0;
        kp := 2.0;
        ti := 120.0;
        td := 0.0;
        auto_mode := TRUE;
    END_IF;

    pid(sp := setpoint, pv := temperature, kp := kp, ti := ti, td := td, dt := 0.1,
        auto := auto_mode, manual_y := manual_heater, y_min := 0.0, y_max := 100.0);
    heater := pid.y;
    at_setpoint := ABS(pid.e) <= 1.0;
END_PROGRAM
