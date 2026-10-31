/**
 * The echo service of the tests, the same seven commands as
 * test/harness/echo.js, so a Node caller can be pointed at either and must
 * see the same answers:
 *
 *   role:<role>,cmd:ping       {code: 200, msg: {echo: args, from: "meta"}}
 *   role:<role>,cmd:fail       {code: 409, err: "conflict on purpose"}
 *   role:<role>,cmd:throw      throws {message: "thrown on purpose", code: "E_PURPOSE"}
 *   role:<role>,cmd:overload   throws with details.message retry_later_err_overload
 *   role:<role>,cmd:slow       {code: 200, msg: "late"} after args.ms (6000)
 *   role:<role>,cmd:delay      what ping answers, after args.ms (0)
 *   role:<role>,cmd:burn       args.n rounds of xorshift64 (500000, about
 *                              1 ms), then {code: 200, msg: {echo, from, h}}
 */
#ifndef MWM_ECHO_H
#define MWM_ECHO_H

void mwm_echoServe(const char *role);

#endif
