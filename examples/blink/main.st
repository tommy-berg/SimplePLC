(* Blink: toggles lamp every blink_ms milliseconds while enable is on.
   Try it: change blink_ms from Modbus, OPC UA or the panel's I/O tab. *)
PROGRAM main
VAR_EXTERNAL
    lamp : BOOL;
    enable : BOOL;
    blink_ms : UINT;
    blinks : UINT;
END_VAR
VAR
    half_period : TON;
END_VAR
    IF FIRST_CYCLE() AND blink_ms = 0 THEN
        blink_ms := 500;
        enable := TRUE;
    END_IF;

    half_period(IN := enable, PT := T#1ms * blink_ms);
    IF NOT enable THEN
        lamp := FALSE;
    ELSIF half_period.Q THEN
        (* Restart the timer in this cycle, so every half period lasts exactly blink_ms *)
        half_period(IN := FALSE);
        half_period(IN := TRUE);
        lamp := NOT lamp;
        IF lamp THEN
            IF blinks = 65535 THEN  (* UINT arithmetic stops the PLC on overflow: wrap on purpose *)
                blinks := 0;
            ELSE
                blinks := blinks + 1;
            END_IF;
        END_IF;
    END_IF;
END_PROGRAM
