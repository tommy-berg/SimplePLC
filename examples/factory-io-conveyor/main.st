(* Factory I/O "From A to B": run the conveyor until a box reaches the end.
   Factory I/O writes the sensor to coil 0 and reads the motor from discrete input 0. *)
PROGRAM main
VAR_EXTERNAL
    box_at_end : BOOL;
    conveyor : BOOL;
END_VAR
    conveyor := NOT box_at_end;
END_PROGRAM
