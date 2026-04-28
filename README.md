Varun Iyer (vmi14) Ahmed Elshenawy (ae508)
chatd.c --> The project implements a multi-client chat server using TCP sockets and the poll() system call. The server supports the following message types:
    1) NAM - this sets the user screen name
    2) SET - this updates the user status
    3) MSG - this sends public/private messages 
    4) WHO - queries user or room information
    5) ERR - outputs error responses 
The chat server was tested across four main categories - that being 1) Core Functionality Tests, 2) Protocol Correctness Tests, 3) Edge Case and boundary tests, 4) Last Field | Tests, and 5) Multi-Client interaction tests. Testing was performed primarily using - printf '...' | nc localhost <port>, which avoids newline issues presented by the interactive nc.
1) Core Functionality Tests: Login (NAM) -> tested using command through bash printf '1|NAM|4|Bob|' | nc localhost 5555, which gives a welcome message. We tested also using a duplicate name as well with command through bash printf '1|NAM|4|Bob|1|NAM|4|Bob|' | nc localhost 5555, which gives us an expected error (ERR 1). Status update (SET) -> we first checked normal status using command through bash printf '1|NAM|4|Bob|1|SET|17||Smiling Politely|' nc localhost 5555, which broadcasts the message to all users in the server. We then tested empty status using command through bash printf '1|NAM|4|Bob|1|SET|1||' | nc localhost 5555, which accepts the message but does not broadcast it.
2) Protocol Correctness Tests: Fatal Errors (ERR 0) -> we used command through bash printf '2|NAM|4|Bob|'
