// Keyboard top module - connects mkeyboard to kbd_event
// This module wraps mkeyboard.v and kbd_event.sv and provides a clean interface

`default_nettype none

module keyboard_top (
    input wire mclk,
    input wire reset,

    // Matrix column inputs (active-low: 0=pressed, 1=released)
    input wire [6:0] keyb_col_i,

    // Row outputs (one-hot, active-low) - driven by mkeyboard
    output wire [7:0] keyb_row_o,

    // 256-bit scancode bitfield
    output wire [255:0] omatrix,

    // Keyboard event queue interface
    output wire [31:0] kbd_push_data,
    output wire        kbd_push_req
);

// Internal signals for connecting mkeyboard to kbd_event
wire [255:0] mkeyboard_omatrix;

// Instantiate mkeyboard - scans matrix and outputs scancodes
mkeyboard mkeyboard_inst (
    .clk     (mclk),
    .reset   (reset),
    .rows_o  (keyb_row_o),
    .cols_i  (keyb_col_i),
    .omatrix (mkeyboard_omatrix)
);

// Generate kbd_matrix_q with bit 0 set if any matrix bit is active
wire [255:0] kbd_matrix_q;
assign kbd_matrix_q[0] = (|mkeyboard_omatrix[255:1]) ? 1'b1 : 1'b0;
assign kbd_matrix_q[255:1] = mkeyboard_omatrix[255:1];

assign omatrix = kbd_matrix_q;

// Instantiate kbd_event - detects key events from matrix
kbd_event kbd_event_inst (
    .mclk             (mclk),
    .reset_mclk_q     (reset),
    .kbd_matrix_q     (kbd_matrix_q),
    .kbd_matrix_q_prev (kbd_matrix_q_prev_reg),
    .kbd_push_data    (kbd_push_data),
    .kbd_push_req     (kbd_push_req)
);

// Register for previous matrix state (needed for edge detection)
reg [255:0] kbd_matrix_q_prev_reg;

always @(posedge mclk or posedge reset) begin
    if (reset) begin
        kbd_matrix_q_prev_reg <= 256'd0;
    end else begin
        // Update previous matrix after kbd_event has compared current vs previous
        // Set bit 0 based on whether any matrix bit is active
        kbd_matrix_q_prev_reg[0] <= (|mkeyboard_omatrix[255:1]) ? 1'b1 : 1'b0;
        kbd_matrix_q_prev_reg[255:1] <= mkeyboard_omatrix[255:1];
    end
end

endmodule

`default_nettype none
