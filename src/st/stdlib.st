(* SimplePLC standard function blocks (IEC 61131-3 Ed. 3, 6.6.3.5).
   Written from the standard's definitions: the edge detectors, bistables and
   counters follow its ST bodies; the timers follow its timing diagrams. Every
   timer reads the cycle's time (one value per cycle) through __NOW().
   Names containing "__" are reserved for this file. *)

FUNCTION_BLOCK R_TRIG
VAR_INPUT CLK : BOOL; END_VAR
VAR_OUTPUT Q : BOOL; END_VAR
VAR M : BOOL; END_VAR
Q := CLK AND NOT M;
M := CLK;
END_FUNCTION_BLOCK

(* As the standard defines it: with CLK FALSE on the first call, Q is TRUE for that call. *)
FUNCTION_BLOCK F_TRIG
VAR_INPUT CLK : BOOL; END_VAR
VAR_OUTPUT Q : BOOL; END_VAR
VAR M : BOOL; END_VAR
Q := NOT CLK AND NOT M;
M := NOT CLK;
END_FUNCTION_BLOCK

(* Set dominant *)
FUNCTION_BLOCK SR
VAR_INPUT S1 : BOOL; R : BOOL; END_VAR
VAR_OUTPUT Q1 : BOOL; END_VAR
Q1 := S1 OR (NOT R AND Q1);
END_FUNCTION_BLOCK

(* Reset dominant *)
FUNCTION_BLOCK RS
VAR_INPUT S : BOOL; R1 : BOOL; END_VAR
VAR_OUTPUT Q1 : BOOL; END_VAR
Q1 := NOT R1 AND (S OR Q1);
END_FUNCTION_BLOCK

(* On-delay: Q goes TRUE when IN has been TRUE for PT; ET counts up to PT. *)
FUNCTION_BLOCK TON
VAR_INPUT IN : BOOL; PT : TIME; END_VAR
VAR_OUTPUT Q : BOOL; ET : TIME; END_VAR
VAR running__ : BOOL; start__ : TIME; END_VAR
IF NOT IN THEN
    Q := FALSE;
    ET := T#0s;
    running__ := FALSE;
ELSE
    IF NOT running__ THEN
        running__ := TRUE;
        start__ := __NOW();
    END_IF;
    ET := __NOW() - start__;
    IF ET >= PT THEN
        ET := MAX(PT, T#0s);
        Q := TRUE;
    ELSE
        Q := FALSE;
    END_IF;
END_IF;
END_FUNCTION_BLOCK

(* Off-delay: Q follows IN going TRUE and stays TRUE for PT after IN goes FALSE. *)
FUNCTION_BLOCK TOF
VAR_INPUT IN : BOOL; PT : TIME; END_VAR
VAR_OUTPUT Q : BOOL; ET : TIME; END_VAR
VAR running__ : BOOL; start__ : TIME; END_VAR
IF IN THEN
    Q := TRUE;
    ET := T#0s;
    running__ := FALSE;
ELSIF Q THEN
    IF NOT running__ THEN
        running__ := TRUE;
        start__ := __NOW();
    END_IF;
    ET := __NOW() - start__;
    IF ET >= PT THEN
        ET := MAX(PT, T#0s);
        Q := FALSE;
        running__ := FALSE;
    END_IF;
END_IF;
END_FUNCTION_BLOCK

(* Pulse: a rising edge of IN gives a pulse of length PT (not retriggerable). *)
FUNCTION_BLOCK TP
VAR_INPUT IN : BOOL; PT : TIME; END_VAR
VAR_OUTPUT Q : BOOL; ET : TIME; END_VAR
VAR running__ : BOOL; start__ : TIME; previous__ : BOOL; END_VAR
IF NOT running__ AND IN AND NOT previous__ THEN
    running__ := TRUE;
    start__ := __NOW();
    Q := TRUE;
END_IF;
IF running__ THEN
    ET := __NOW() - start__;
    IF ET >= PT THEN
        ET := MAX(PT, T#0s);
        Q := FALSE;
        running__ := FALSE;
    END_IF;
END_IF;
IF NOT IN AND NOT running__ THEN
    ET := T#0s;
END_IF;
previous__ := IN;
END_FUNCTION_BLOCK

(* Counters: CU and CD count rising edges (R_EDGE in the standard); R has priority over LD. *)
FUNCTION_BLOCK CTU
VAR_INPUT CU : BOOL; R : BOOL; PV : INT; END_VAR
VAR_OUTPUT Q : BOOL; CV : INT; END_VAR
VAR cu__ : R_TRIG; END_VAR
cu__(CLK := CU);
IF R THEN
    CV := 0;
ELSIF cu__.Q AND (CV < 32767) THEN
    CV := CV + 1;
END_IF;
Q := CV >= PV;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTU_DINT
VAR_INPUT CU : BOOL; R : BOOL; PV : DINT; END_VAR
VAR_OUTPUT Q : BOOL; CV : DINT; END_VAR
VAR cu__ : R_TRIG; END_VAR
cu__(CLK := CU);
IF R THEN
    CV := 0;
ELSIF cu__.Q AND (CV < 2147483647) THEN
    CV := CV + 1;
END_IF;
Q := CV >= PV;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTU_LINT
VAR_INPUT CU : BOOL; R : BOOL; PV : LINT; END_VAR
VAR_OUTPUT Q : BOOL; CV : LINT; END_VAR
VAR cu__ : R_TRIG; END_VAR
cu__(CLK := CU);
IF R THEN
    CV := 0;
ELSIF cu__.Q AND (CV < 9223372036854775807) THEN
    CV := CV + 1;
END_IF;
Q := CV >= PV;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTU_UDINT
VAR_INPUT CU : BOOL; R : BOOL; PV : UDINT; END_VAR
VAR_OUTPUT Q : BOOL; CV : UDINT; END_VAR
VAR cu__ : R_TRIG; END_VAR
cu__(CLK := CU);
IF R THEN
    CV := 0;
ELSIF cu__.Q AND (CV < 4294967295) THEN
    CV := CV + 1;
END_IF;
Q := CV >= PV;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTU_ULINT
VAR_INPUT CU : BOOL; R : BOOL; PV : ULINT; END_VAR
VAR_OUTPUT Q : BOOL; CV : ULINT; END_VAR
VAR cu__ : R_TRIG; END_VAR
cu__(CLK := CU);
IF R THEN
    CV := 0;
ELSIF cu__.Q AND (CV < 18446744073709551615) THEN
    CV := CV + 1;
END_IF;
Q := CV >= PV;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTD
VAR_INPUT CD : BOOL; LD : BOOL; PV : INT; END_VAR
VAR_OUTPUT Q : BOOL; CV : INT; END_VAR
VAR cd__ : R_TRIG; END_VAR
cd__(CLK := CD);
IF LD THEN
    CV := PV;
ELSIF cd__.Q AND (CV > -32768) THEN
    CV := CV - 1;
END_IF;
Q := CV <= 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTD_DINT
VAR_INPUT CD : BOOL; LD : BOOL; PV : DINT; END_VAR
VAR_OUTPUT Q : BOOL; CV : DINT; END_VAR
VAR cd__ : R_TRIG; END_VAR
cd__(CLK := CD);
IF LD THEN
    CV := PV;
ELSIF cd__.Q AND (CV > -2147483648) THEN
    CV := CV - 1;
END_IF;
Q := CV <= 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTD_LINT
VAR_INPUT CD : BOOL; LD : BOOL; PV : LINT; END_VAR
VAR_OUTPUT Q : BOOL; CV : LINT; END_VAR
VAR cd__ : R_TRIG; END_VAR
cd__(CLK := CD);
IF LD THEN
    CV := PV;
ELSIF cd__.Q AND (CV > -9223372036854775807 - 1) THEN
    CV := CV - 1;
END_IF;
Q := CV <= 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTD_UDINT
VAR_INPUT CD : BOOL; LD : BOOL; PV : UDINT; END_VAR
VAR_OUTPUT Q : BOOL; CV : UDINT; END_VAR
VAR cd__ : R_TRIG; END_VAR
cd__(CLK := CD);
IF LD THEN
    CV := PV;
ELSIF cd__.Q AND (CV > 0) THEN
    CV := CV - 1;
END_IF;
Q := CV = 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTD_ULINT
VAR_INPUT CD : BOOL; LD : BOOL; PV : ULINT; END_VAR
VAR_OUTPUT Q : BOOL; CV : ULINT; END_VAR
VAR cd__ : R_TRIG; END_VAR
cd__(CLK := CD);
IF LD THEN
    CV := PV;
ELSIF cd__.Q AND (CV > 0) THEN
    CV := CV - 1;
END_IF;
Q := CV = 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTUD
VAR_INPUT CU : BOOL; CD : BOOL; R : BOOL; LD : BOOL; PV : INT; END_VAR
VAR_OUTPUT QU : BOOL; QD : BOOL; CV : INT; END_VAR
VAR cu__ : R_TRIG; cd__ : R_TRIG; END_VAR
cu__(CLK := CU);
cd__(CLK := CD);
IF R THEN
    CV := 0;
ELSIF LD THEN
    CV := PV;
ELSIF NOT (cu__.Q AND cd__.Q) THEN
    IF cu__.Q AND (CV < 32767) THEN
        CV := CV + 1;
    ELSIF cd__.Q AND (CV > -32768) THEN
        CV := CV - 1;
    END_IF;
END_IF;
QU := CV >= PV;
QD := CV <= 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTUD_DINT
VAR_INPUT CU : BOOL; CD : BOOL; R : BOOL; LD : BOOL; PV : DINT; END_VAR
VAR_OUTPUT QU : BOOL; QD : BOOL; CV : DINT; END_VAR
VAR cu__ : R_TRIG; cd__ : R_TRIG; END_VAR
cu__(CLK := CU);
cd__(CLK := CD);
IF R THEN
    CV := 0;
ELSIF LD THEN
    CV := PV;
ELSIF NOT (cu__.Q AND cd__.Q) THEN
    IF cu__.Q AND (CV < 2147483647) THEN
        CV := CV + 1;
    ELSIF cd__.Q AND (CV > -2147483648) THEN
        CV := CV - 1;
    END_IF;
END_IF;
QU := CV >= PV;
QD := CV <= 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTUD_LINT
VAR_INPUT CU : BOOL; CD : BOOL; R : BOOL; LD : BOOL; PV : LINT; END_VAR
VAR_OUTPUT QU : BOOL; QD : BOOL; CV : LINT; END_VAR
VAR cu__ : R_TRIG; cd__ : R_TRIG; END_VAR
cu__(CLK := CU);
cd__(CLK := CD);
IF R THEN
    CV := 0;
ELSIF LD THEN
    CV := PV;
ELSIF NOT (cu__.Q AND cd__.Q) THEN
    IF cu__.Q AND (CV < 9223372036854775807) THEN
        CV := CV + 1;
    ELSIF cd__.Q AND (CV > -9223372036854775807 - 1) THEN
        CV := CV - 1;
    END_IF;
END_IF;
QU := CV >= PV;
QD := CV <= 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTUD_UDINT
VAR_INPUT CU : BOOL; CD : BOOL; R : BOOL; LD : BOOL; PV : UDINT; END_VAR
VAR_OUTPUT QU : BOOL; QD : BOOL; CV : UDINT; END_VAR
VAR cu__ : R_TRIG; cd__ : R_TRIG; END_VAR
cu__(CLK := CU);
cd__(CLK := CD);
IF R THEN
    CV := 0;
ELSIF LD THEN
    CV := PV;
ELSIF NOT (cu__.Q AND cd__.Q) THEN
    IF cu__.Q AND (CV < 4294967295) THEN
        CV := CV + 1;
    ELSIF cd__.Q AND (CV > 0) THEN
        CV := CV - 1;
    END_IF;
END_IF;
QU := CV >= PV;
QD := CV = 0;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTUD_ULINT
VAR_INPUT CU : BOOL; CD : BOOL; R : BOOL; LD : BOOL; PV : ULINT; END_VAR
VAR_OUTPUT QU : BOOL; QD : BOOL; CV : ULINT; END_VAR
VAR cu__ : R_TRIG; cd__ : R_TRIG; END_VAR
cu__(CLK := CU);
cd__(CLK := CD);
IF R THEN
    CV := 0;
ELSIF LD THEN
    CV := PV;
ELSIF NOT (cu__.Q AND cd__.Q) THEN
    IF cu__.Q AND (CV < 18446744073709551615) THEN
        CV := CV + 1;
    ELSIF cd__.Q AND (CV > 0) THEN
        CV := CV - 1;
    END_IF;
END_IF;
QU := CV >= PV;
QD := CV = 0;
END_FUNCTION_BLOCK
