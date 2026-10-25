(* Traffic light with a pedestrian button.

   Phases: GREEN -> YELLOW -> RED -> WALK (red + walk) -> RED_YELLOW -> GREEN.
   Green lasts at least green_ms and then waits for the button. *)
TYPE
    Light : (GREEN_PHASE, YELLOW_PHASE, RED_PHASE, WALK_PHASE, RED_YELLOW_PHASE);
END_TYPE

PROGRAM main
VAR_EXTERNAL
    red : BOOL;
    yellow : BOOL;
    green : BOOL;
    walk : BOOL;
    button : BOOL;
    phase : UINT;
    green_ms : UINT;
END_VAR
VAR
    state : Light;  (* starts with the first value, GREEN_PHASE *)
    next : Light;
    in_phase : TON;
END_VAR
    IF FIRST_CYCLE() AND green_ms = 0 THEN
        green_ms := 4000;
    END_IF;

    next := state;
    CASE state OF
        GREEN_PHASE:
            in_phase(IN := TRUE, PT := T#1ms * green_ms);
            IF in_phase.Q AND button THEN
                button := FALSE;
                next := YELLOW_PHASE;
            END_IF;
        YELLOW_PHASE:
            in_phase(IN := TRUE, PT := T#3s);
            IF in_phase.Q THEN
                next := RED_PHASE;
            END_IF;
        RED_PHASE:
            in_phase(IN := TRUE, PT := T#1s);
            IF in_phase.Q THEN
                next := WALK_PHASE;
            END_IF;
        WALK_PHASE:
            in_phase(IN := TRUE, PT := T#5s);
            IF in_phase.Q THEN
                next := RED_YELLOW_PHASE;
            END_IF;
        RED_YELLOW_PHASE:
            in_phase(IN := TRUE, PT := T#1500ms);
            IF in_phase.Q THEN
                next := GREEN_PHASE;
            END_IF;
    END_CASE;
    IF next <> state THEN
        state := next;
        (* Restart the timer in this cycle, so each phase lasts exactly its time *)
        in_phase(IN := FALSE);
        in_phase(IN := TRUE);
    END_IF;

    green := state = GREEN_PHASE;
    yellow := state = YELLOW_PHASE OR state = RED_YELLOW_PHASE;
    red := state = RED_PHASE OR state = WALK_PHASE OR state = RED_YELLOW_PHASE;
    walk := state = WALK_PHASE;
    CASE state OF
        GREEN_PHASE: phase := 0;
        YELLOW_PHASE: phase := 1;
        RED_PHASE: phase := 2;
        WALK_PHASE: phase := 3;
        RED_YELLOW_PHASE: phase := 4;
    END_CASE;
END_PROGRAM
