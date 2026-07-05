// src/compiler/scanner.ts
function makeToken(type, lexeme, literal, line, position) {
  return { type, lexeme, literal, line, position };
}
var KEYWORDS = {
  datablock: 0 /* Datablock */,
  package: 1 /* Package */,
  function: 2 /* Function */,
  if: 3 /* If */,
  else: 4 /* Else */,
  while: 10 /* While */,
  for: 11 /* For */,
  break: 8 /* Break */,
  continue: 45 /* Continue */,
  case: 6 /* Case */,
  switch: 5 /* Switch */,
  return: 7 /* Return */,
  new: 9 /* New */,
  true: 12 /* True */,
  false: 13 /* False */,
  default: 14 /* Default */,
  or: 72 /* Or */
};
var Scanner = class {
  constructor(source) {
    this.tokens = [];
    this.start = 0;
    this.current = 0;
    this.line = 1;
    this.source = source;
  }
  scanTokens() {
    while (!this.isAtEnd()) {
      this.start = this.current;
      this.scanToken();
    }
    this.tokens.push({ type: 75 /* Eof */, lexeme: "", literal: null, line: this.line, position: this.current });
    return this.tokens;
  }
  isAtEnd() {
    return this.current >= this.source.length;
  }
  scanToken() {
    const c = this.advance();
    switch (c) {
      case "(":
        this.addToken(59 /* LParen */);
        break;
      case ")":
        this.addToken(60 /* RParen */);
        break;
      case "{":
        this.addToken(61 /* LBracket */);
        break;
      case "}":
        this.addToken(62 /* RBracket */);
        break;
      case ",":
        this.addToken(63 /* Comma */);
        break;
      case ";":
        this.addToken(64 /* Semicolon */);
        break;
      case "[":
        this.addToken(67 /* LeftSquareBracket */);
        break;
      case "]":
        this.addToken(68 /* RightSquareBracket */);
        break;
      case ".":
        this.addToken(70 /* Dot */);
        break;
      case "?":
        this.addToken(71 /* QuestionMark */);
        break;
      case "~":
        this.addToken(38 /* Tilde */);
        break;
      case "^":
        this.addToken(this.match("=") ? 26 /* XorAssign */ : 52 /* BitwiseXor */);
        break;
      case "%":
        this.addToken(this.match("=") ? 27 /* ModulusAssign */ : 19 /* Modulus */);
        break;
      case ":":
        this.addToken(this.match(":") ? 66 /* DoubleColon */ : 65 /* Colon */);
        break;
      case "+":
        if (this.match("=")) this.addToken(21 /* PlusAssign */);
        else if (this.match("+")) this.addToken(73 /* PlusPlus */);
        else this.addToken(15 /* Plus */);
        break;
      case "-":
        if (this.match("=")) this.addToken(22 /* MinusAssign */);
        else if (this.match("-")) this.addToken(74 /* MinusMinus */);
        else this.addToken(16 /* Minus */);
        break;
      case "*":
        this.addToken(this.match("=") ? 23 /* MultiplyAssign */ : 17 /* Multiply */);
        break;
      case "/":
        if (this.match("/")) {
          while (this.peek() !== "\n" && !this.isAtEnd()) this.advance();
        } else if (this.match("*")) {
          while (!(this.peek() === "*" && this.peekNext() === "/") && !this.isAtEnd()) {
            if (this.peek() === "\n") this.line++;
            this.advance();
          }
          if (!this.isAtEnd()) {
            this.advance();
            this.advance();
          }
        } else {
          this.addToken(this.match("=") ? 28 /* DivideAssign */ : 18 /* Divide */);
        }
        break;
      case "@":
        this.addToken(41 /* Concat */);
        break;
      case "$":
        this.addToken(this.match("=") ? 39 /* StringEquals */ : 69 /* Dollar */);
        break;
      case "&":
        if (this.match("=")) this.addToken(25 /* AndAssign */);
        else if (this.match("&")) this.addToken(46 /* LogicalAnd */);
        else this.addToken(50 /* BitwiseAnd */);
        break;
      case "|":
        if (this.match("=")) this.addToken(24 /* OrAssign */);
        else if (this.match("|")) this.addToken(47 /* LogicalOr */);
        else this.addToken(51 /* BitwiseOr */);
        break;
      case "!":
        if (this.peek() === "$" && this.peekNext() === "=") {
          this.advance();
          this.advance();
          this.addToken(40 /* StringNotEquals */);
        } else if (this.match("=")) {
          this.addToken(36 /* NotEqual */);
        } else {
          this.addToken(35 /* Not */);
        }
        break;
      case "=":
        this.addToken(this.match("=") ? 37 /* Equal */ : 20 /* Assign */);
        break;
      case "<":
        if (this.match("=")) this.addToken(33 /* LessThanEqual */);
        else if (this.match("<")) this.addToken(this.match("=") ? 29 /* ShiftLeftAssign */ : 48 /* LeftBitShift */);
        else this.addToken(31 /* LessThan */);
        break;
      case ">":
        if (this.match("=")) this.addToken(34 /* GreaterThanEqual */);
        else if (this.match(">")) this.addToken(this.match("=") ? 30 /* ShiftRightAssign */ : 49 /* RightBitShift */);
        else this.addToken(32 /* GreaterThan */);
        break;
      case '"':
        this.readString('"');
        break;
      case "'":
        this.readString("'");
        break;
      case " ":
      case "\r":
      case "	":
        break;
      case "\n":
        this.line++;
        break;
      default:
        if (this.isDigit(c)) {
          this.readNumber(c);
        } else if (this.isAlpha(c)) {
          this.readIdentifier();
        }
        break;
    }
  }
  readString(delimiter) {
    let escaped = false;
    while ((this.peek() !== delimiter || escaped) && !this.isAtEnd()) {
      if (this.peek() === "\n") this.line++;
      if (this.peek() === "\\" && !escaped) escaped = true;
      else escaped = false;
      this.advance();
    }
    if (this.isAtEnd()) return;
    this.advance();
    const raw = this.source.substring(this.start + 1, this.current - 1);
    const value = this.unescape(raw);
    const type = delimiter === "'" ? 58 /* TaggedString */ : 57 /* String */;
    this.addToken(type, value);
  }
  unescape(s) {
    const map = [
      ["\\\\", "\\"],
      ["\\'", "'"],
      ['\\"', '"'],
      ["\\t", "	"],
      ["\\n", "\n"],
      ["\\r", "\r"]
    ];
    for (const [from, to] of map) {
      s = s.split(from).join(to);
    }
    return s.replace(/\\x([0-9a-fA-F]{2})/g, (_, hex) => String.fromCharCode(parseInt(hex, 16)));
  }
  readNumber(first) {
    let isFloat = false;
    if (first === "0" && (this.peek() === "x" || this.peek() === "X")) {
      this.advance();
      while (this.isHexDigit(this.peek())) this.advance();
      this.addToken(55 /* HexInt */, this.source.substring(this.start, this.current));
      return;
    }
    while (this.isDigit(this.peek())) this.advance();
    if (this.peek() === "." && this.isDigit(this.peekNext())) {
      isFloat = true;
      this.advance();
      while (this.isDigit(this.peek())) this.advance();
    }
    if (this.peek() === "e" || this.peek() === "E") {
      isFloat = true;
      this.advance();
      if (this.peek() === "+" || this.peek() === "-") this.advance();
      while (this.isDigit(this.peek())) this.advance();
    }
    this.addToken(
      isFloat ? 56 /* Float */ : 54 /* Int */,
      isFloat ? parseFloat(this.source.substring(this.start, this.current)) : parseInt(this.source.substring(this.start, this.current), 10)
    );
  }
  readIdentifier() {
    while (this.isAlphaNumeric(this.peek())) this.advance();
    const text = this.source.substring(this.start, this.current);
    const kw = KEYWORDS[text];
    if (kw !== void 0) {
      this.addToken(kw, text);
    } else if (text === "SPC") {
      this.addToken(42 /* SpaceConcat */, text);
    } else if (text === "TAB") {
      this.addToken(43 /* TabConcat */, text);
    } else if (text === "NL") {
      this.addToken(44 /* NewlineConcat */, text);
    } else {
      this.addToken(53 /* Label */, text);
    }
  }
  advance() {
    return this.source[this.current++];
  }
  peek() {
    return this.isAtEnd() ? "\0" : this.source[this.current];
  }
  peekNext() {
    return this.current + 1 >= this.source.length ? "\0" : this.source[this.current + 1];
  }
  match(expected) {
    if (this.isAtEnd() || this.source[this.current] !== expected) return false;
    this.current++;
    return true;
  }
  addToken(type, literal = null) {
    const lexeme = this.source.substring(this.start, this.current);
    this.tokens.push({ type, lexeme, literal, line: this.line, position: this.start });
  }
  isDigit(c) {
    return c >= "0" && c <= "9";
  }
  isHexDigit(c) {
    return this.isDigit(c) || c >= "a" && c <= "f" || c >= "A" && c <= "F";
  }
  isAlpha(c) {
    return c >= "a" && c <= "z" || c >= "A" && c <= "Z" || c === "_";
  }
  isAlphaNumeric(c) {
    return this.isAlpha(c) || this.isDigit(c);
  }
};

// src/compiler/ast.ts
var Stmt = class {
  constructor(lineNo) {
    this.lineNo = lineNo;
  }
};
var Expr = class extends Stmt {
  constructor(lineNo) {
    super(lineNo);
  }
};
var BreakStmt = class extends Stmt {
  constructor(lineNo) {
    super(lineNo);
  }
};
var ContinueStmt = class extends Stmt {
  constructor(lineNo) {
    super(lineNo);
  }
};
var ReturnStmt = class extends Stmt {
  constructor(lineNo, expr) {
    super(lineNo);
    this.expr = expr;
  }
};
var IfStmt = class extends Stmt {
  constructor(lineNo, condition, body, elseBody) {
    super(lineNo);
    this.condition = condition;
    this.body = body;
    this.elseBody = elseBody;
  }
};
var LoopStmt = class extends Stmt {
  constructor(lineNo, condition, init, end, body) {
    super(lineNo);
    this.condition = condition;
    this.init = init;
    this.end = end;
    this.body = body;
  }
};
var FunctionDeclStmt = class extends Stmt {
  constructor(functionName, args2, stmts, namespace) {
    super(functionName.line);
    this.functionName = functionName;
    this.packageName = null;
    this.args = args2;
    this.stmts = stmts;
    this.namespace = namespace;
  }
};
var PackageDeclStmt = class extends Stmt {
  constructor(name, functions) {
    super(name.line);
    this.name = name;
    this.functions = functions;
  }
};
var ParenthesisExpr = class extends Expr {
  constructor(expr) {
    super(expr.lineNo);
    this.expr = expr;
  }
};
var FloatBinaryExpr = class extends Expr {
  constructor(left, right, op) {
    super(left.lineNo);
    this.left = left;
    this.right = right;
    this.op = op;
  }
};
var IntBinaryExpr = class extends Expr {
  constructor(left, right, op) {
    super(left.lineNo);
    this.left = left;
    this.right = right;
    this.op = op;
  }
};
var StrEqExpr = class extends Expr {
  constructor(left, right, op) {
    super(left.lineNo);
    this.left = left;
    this.right = right;
    this.op = op;
  }
};
var StrCatExpr = class extends Expr {
  constructor(left, right, op) {
    super(left.lineNo);
    this.left = left;
    this.right = right;
    this.op = op;
  }
};
var ConditionalExpr = class extends Expr {
  constructor(condition, trueExpr, falseExpr) {
    super(condition.lineNo);
    this.condition = condition;
    this.trueExpr = trueExpr;
    this.falseExpr = falseExpr;
  }
};
var IntUnaryExpr = class extends Expr {
  constructor(expr, op) {
    super(expr.lineNo);
    this.expr = expr;
    this.op = op;
  }
};
var FloatUnaryExpr = class extends Expr {
  constructor(expr, op) {
    super(expr.lineNo);
    this.expr = expr;
    this.op = op;
  }
};
var VarExpr = class extends Expr {
  constructor(name, namespace, arrayIndex, vtype) {
    super(name.line);
    this.name = name;
    this.namespace = namespace;
    this.arrayIndex = arrayIndex;
    this.vtype = vtype;
  }
};
var IntExpr = class extends Expr {
  constructor(lineNo, value) {
    super(lineNo);
    this.value = value;
  }
};
var FloatExpr = class extends Expr {
  constructor(lineNo, value) {
    super(lineNo);
    this.value = value;
  }
};
var StringConstExpr = class extends Expr {
  constructor(lineNo, value, tag) {
    super(lineNo);
    this.value = value;
    this.tag = tag;
  }
};
var ConstantExpr = class extends Expr {
  constructor(name) {
    super(name.line);
    this.name = name;
  }
};
var AssignExpr = class extends Expr {
  constructor(varExpr, expr) {
    super(varExpr.lineNo);
    this.varExpr = varExpr;
    this.expr = expr;
  }
};
var AssignOpExpr = class extends Expr {
  constructor(varExpr, expr, op) {
    super(varExpr.lineNo);
    this.varExpr = varExpr;
    this.expr = expr;
    this.op = op;
  }
};
var FuncCallExpr = class extends Expr {
  constructor(name, namespace, args2, callType, objectExpr = null) {
    super(name.line);
    this.name = name;
    this.namespace = namespace;
    this.args = args2;
    this.callType = callType;
    this.objectExpr = objectExpr;
  }
};
var SlotAccessExpr = class extends Expr {
  constructor(objectExpr, arrayExpr, slotName) {
    super(objectExpr.lineNo);
    this.objectExpr = objectExpr;
    this.arrayExpr = arrayExpr;
    this.slotName = slotName;
  }
};
var SlotAssignExpr = class extends Expr {
  constructor(objectExpr, arrayExpr, slotName, expr) {
    super(slotName ? slotName.line : objectExpr ? objectExpr.lineNo : 0);
    this.objectExpr = objectExpr;
    this.arrayExpr = arrayExpr;
    this.slotName = slotName;
    this.expr = expr;
  }
};
var SlotAssignOpExpr = class extends Expr {
  constructor(objectExpr, arrayExpr, slotName, expr, op) {
    super(objectExpr.lineNo);
    this.objectExpr = objectExpr;
    this.arrayExpr = arrayExpr;
    this.slotName = slotName;
    this.expr = expr;
    this.op = op;
  }
};
var ObjectDeclExpr = class extends Expr {
  constructor(className, parentObject, objectNameExpr, args2, slotDecls, subObjects, structDecl) {
    super(className.lineNo);
    this.className = className;
    this.parentObject = parentObject;
    this.objectNameExpr = objectNameExpr;
    this.args = args2;
    this.slotDecls = slotDecls;
    this.subObjects = subObjects;
    this.structDecl = structDecl;
  }
};

// src/compiler/parser.ts
var SyntaxError = class extends Error {
  constructor(message, token) {
    super(`${message} at line ${token.line}`);
    this.token = token;
  }
};
var Parser = class {
  constructor(tokens) {
    this.current = 0;
    this.tokens = tokens.filter((t) => t.type !== 76 /* Comment */);
  }
  parse() {
    const stmts = [];
    while (!this.isAtEnd()) {
      try {
        const s = this.decl();
        if (s) stmts.push(s);
      } catch (e) {
        while (!this.isAtEnd()) {
          if (this.check(62 /* RBracket */) || this.check(2 /* Function */) || this.check(1 /* Package */) || this.check(75 /* Eof */)) break;
          this.advance();
        }
        if (!this.isAtEnd()) this.advance();
      }
    }
    return stmts;
  }
  decl() {
    if (this.match(1 /* Package */)) {
      return this.packageDecl();
    }
    const fn = this.functionDecl();
    if (fn) return fn;
    return this.stmt();
  }
  packageDecl() {
    const name = this.consume(53 /* Label */, "Expected package name");
    this.consume(61 /* LBracket */, "Expected '{' after package name");
    const fns = [];
    while (this.check(2 /* Function */)) {
      const fn = this.functionDecl();
      if (fn) {
        fn.packageName = name;
        fns.push(fn);
      }
    }
    this.consume(62 /* RBracket */, "Expected '}' after package body");
    this.tryConsume(64 /* Semicolon */);
    return new PackageDeclStmt(name, fns);
  }
  functionDecl() {
    if (!this.match(2 /* Function */)) return null;
    const fnName = this.consume(53 /* Label */, "Expected function name");
    let namespace = null;
    if (this.match(66 /* DoubleColon */)) {
      namespace = fnName;
      this.consume(53 /* Label */, "Expected function name after ::");
    }
    this.consume(59 /* LParen */, "Expected '(' after function name");
    const args2 = [];
    if (!this.check(60 /* RParen */)) {
      const v = this.variable();
      if (v) args2.push(v);
      while (this.match(63 /* Comma */)) {
        const v2 = this.variable();
        if (v2) args2.push(v2);
      }
    }
    this.consume(60 /* RParen */, "Expected ')' after parameters");
    this.consume(61 /* LBracket */, "Expected '{' before function body");
    const body = this.statementList();
    this.consume(62 /* RBracket */, "Expected '}' after function body");
    return new FunctionDeclStmt(fnName, args2, body, namespace);
  }
  variable() {
    if (this.match(69 /* Dollar */) || this.match(19 /* Modulus */)) {
      const isGlobal = this.previous().type === 69 /* Dollar */;
      return this.parseVariableNameToken(isGlobal);
    }
    return null;
  }
  parseVariableNameToken(isGlobal) {
    const nextToken = this.peek();
    const canBeName = nextToken.type === 53 /* Label */ || nextToken.type === 54 /* Int */ || nextToken.type === 56 /* Float */ || nextToken.type >= 0 /* Datablock */ && nextToken.type <= 14 /* Default */ || nextToken.type === 45 /* Continue */ || nextToken.type === 72 /* Or */;
    if (canBeName) {
      let nameToken = this.advance();
      if (nameToken.literal === null || nameToken.literal === void 0) {
        nameToken = {
          ...nameToken,
          literal: nameToken.lexeme
        };
      } else {
        nameToken = {
          ...nameToken,
          literal: String(nameToken.literal)
        };
      }
      return new VarExpr(nameToken, null, null, isGlobal ? 0 /* Global */ : 1 /* Local */);
    }
    throw new SyntaxError("Expected variable name", nextToken);
  }
  parseArrayIndex() {
    let expr = this.expression();
    const line = this.previous().line;
    const concatOp = { type: 41 /* Concat */, lexeme: "@", literal: null, line, position: 0 };
    while (this.match(63 /* Comma */)) {
      const right = this.expression();
      const underscore = new StringConstExpr(line, "_", false);
      const intermediate = new StrCatExpr(expr, underscore, concatOp);
      expr = new StrCatExpr(intermediate, right, concatOp);
    }
    return expr;
  }
  statementList() {
    const stmts = [];
    while (!this.check(62 /* RBracket */) && !this.isAtEnd() && !this.check(6 /* Case */) && !this.check(14 /* Default */)) {
      const s = this.stmt();
      if (s) stmts.push(s);
    }
    return stmts;
  }
  stmt() {
    if (this.check(8 /* Break */)) return this.breakStmt();
    if (this.check(45 /* Continue */)) return this.continueStmt();
    if (this.check(7 /* Return */)) return this.returnStmt();
    if (this.check(3 /* If */)) return this.ifStmt();
    if (this.check(10 /* While */)) return this.whileStmt();
    if (this.check(11 /* For */)) return this.forStmt();
    if (this.check(5 /* Switch */)) return this.switchStmt();
    if (this.check(0 /* Datablock */)) return this.datablockStmt();
    return this.expressionStmt();
  }
  breakStmt() {
    const line = this.peek().line;
    this.advance();
    this.consume(64 /* Semicolon */, "Expected ';' after break");
    return new BreakStmt(line);
  }
  continueStmt() {
    const line = this.peek().line;
    this.advance();
    this.consume(64 /* Semicolon */, "Expected ';' after continue");
    return new ContinueStmt(line);
  }
  returnStmt() {
    const line = this.peek().line;
    this.advance();
    if (this.match(64 /* Semicolon */)) return new ReturnStmt(line, null);
    const expr = this.expression();
    this.consume(64 /* Semicolon */, "Expected ';' after return");
    return new ReturnStmt(line, expr);
  }
  ifStmt() {
    const line = this.peek().line;
    this.advance();
    this.consume(59 /* LParen */, "Expected '(' after if");
    const cond = this.expression();
    this.tryConsume(60 /* RParen */);
    const body = this.blockStatements();
    let elseBody = null;
    if (this.match(4 /* Else */)) {
      elseBody = this.blockStatements();
    }
    return new IfStmt(line, cond, body, elseBody);
  }
  whileStmt() {
    const line = this.peek().line;
    this.advance();
    this.consume(59 /* LParen */, "Expected '(' after while");
    const cond = this.expression();
    this.tryConsume(60 /* RParen */);
    const body = this.blockStatements();
    return new LoopStmt(line, cond, null, null, body);
  }
  forStmt() {
    const line = this.peek().line;
    this.advance();
    this.consume(59 /* LParen */, "Expected '(' after for");
    let init = null;
    if (!this.check(64 /* Semicolon */)) {
      init = this.stmtExpr();
      if (!init) init = this.expression();
    }
    this.tryConsume(64 /* Semicolon */);
    const cond = this.expression();
    this.tryConsume(64 /* Semicolon */);
    let end = null;
    if (!this.check(60 /* RParen */)) {
      end = this.stmtExpr();
      if (!end) end = this.expression();
    }
    this.tryConsume(60 /* RParen */);
    const body = this.blockStatements();
    return new LoopStmt(line, cond, init, end, body);
  }
  switchStmt() {
    const line = this.peek().line;
    this.advance();
    let isStringSwitch = false;
    if (this.match(69 /* Dollar */)) isStringSwitch = true;
    this.consume(59 /* LParen */, "Expected '(' after switch");
    const expr = this.expression();
    this.consume(60 /* RParen */, "Expected ')' after switch expression");
    this.consume(61 /* LBracket */, "Expected '{' before switch body");
    const cases = this.caseBlock(expr, isStringSwitch);
    this.consume(62 /* RBracket */, "Expected '}' after switch body");
    return cases;
  }
  caseBlock(switchExpr, isStringSwitch) {
    this.consume(6 /* Case */, "Expected 'case'");
    const conditions = [this.expression()];
    while (this.match(72 /* Or */)) {
      conditions.push(this.expression());
    }
    this.consume(65 /* Colon */, "Expected ':' after case expression");
    const stmts = this.statementList();
    let checkExpr;
    if (isStringSwitch) {
      checkExpr = new StrEqExpr(switchExpr, conditions[0], makeToken(39 /* StringEquals */, "$=", "$=", 0, 0));
      for (let i = 1; i < conditions.length; i++) {
        checkExpr = new IntBinaryExpr(
          checkExpr,
          new StrEqExpr(switchExpr, conditions[i], makeToken(39 /* StringEquals */, "$=", "$=", 0, 0)),
          makeToken(47 /* LogicalOr */, "||", "||", 0, 0)
        );
      }
    } else {
      checkExpr = new IntBinaryExpr(switchExpr, conditions[0], makeToken(37 /* Equal */, "==", "==", 0, 0));
      for (let i = 1; i < conditions.length; i++) {
        checkExpr = new IntBinaryExpr(
          checkExpr,
          new IntBinaryExpr(switchExpr, conditions[i], makeToken(37 /* Equal */, "==", "==", 0, 0)),
          makeToken(47 /* LogicalOr */, "||", "||", 0, 0)
        );
      }
    }
    const ifStmt = new IfStmt(checkExpr.lineNo, checkExpr, stmts, null);
    if (this.check(6 /* Case */)) {
      const nextCase = this.caseBlock(switchExpr, isStringSwitch);
      ifStmt.elseBody = [nextCase];
    } else if (this.check(14 /* Default */)) {
      this.advance();
      this.consume(65 /* Colon */, "Expected ':' after default");
      ifStmt.elseBody = this.statementList();
    }
    return ifStmt;
  }
  datablockStmt() {
    this.advance();
    const className = this.consume(53 /* Label */, "Expected class name");
    this.consume(59 /* LParen */, "Expected '(' after datablock name");
    const name = this.consume(53 /* Label */, "Expected datablock name");
    let parentName = null;
    if (this.match(65 /* Colon */)) {
      parentName = this.consume(53 /* Label */, "Expected parent name");
    }
    this.consume(60 /* RParen */, "Expected ')' after datablock name");
    this.consume(61 /* LBracket */, "Expected '{' before datablock body");
    const slots = [];
    while (!this.check(62 /* RBracket */) && !this.isAtEnd()) {
      const slot = this.slotAssign();
      if (slot) slots.push(slot);
      else break;
    }
    this.consume(62 /* RBracket */, "Expected '}' after datablock body");
    this.tryConsume(64 /* Semicolon */);
    const result = new ObjectDeclExpr(
      new ConstantExpr(className),
      parentName,
      new ConstantExpr(name),
      [],
      slots,
      [],
      true
    );
    return result;
  }
  slotAssign() {
    if (this.check(53 /* Label */)) {
      const slotName = this.advance();
      let arrayIdx = null;
      if (this.match(67 /* LeftSquareBracket */)) {
        arrayIdx = this.parseArrayIndex();
        this.consume(68 /* RightSquareBracket */, "Expected ']' after array index");
      }
      this.consume(20 /* Assign */, "Expected '=' after slot name");
      const expr = this.expression();
      this.tryConsume(64 /* Semicolon */);
      return new SlotAssignExpr(null, arrayIdx, slotName, expr);
    }
    return null;
  }
  blockStatements() {
    if (this.match(61 /* LBracket */)) {
      const stmts = this.statementList();
      this.consume(62 /* RBracket */, "Expected '}' after block");
      return stmts;
    }
    const s = this.stmt();
    return s ? [s] : [];
  }
  expressionStmt() {
    const expr = this.stmtExpr();
    if (expr) {
      this.tryConsume(64 /* Semicolon */);
    }
    return expr;
  }
  stmtExpr() {
    const expr = this.expression();
    if (!expr) return null;
    if (this.match(70 /* Dot */)) {
      const label = this.consume(53 /* Label */, "Expected label after .");
      let arrAccess = null;
      if (this.match(67 /* LeftSquareBracket */)) {
        arrAccess = this.parseArrayIndex();
        this.consume(68 /* RightSquareBracket */, "Expected ']' after array index");
      }
      if (this.match(20 /* Assign */)) {
        const rexpr = this.expression();
        return new SlotAssignExpr(expr, arrAccess, label, rexpr);
      }
      if (this.check(73 /* PlusPlus */) || this.check(74 /* MinusMinus */)) {
        const op = this.advance();
        return new ParenthesisExpr(new SlotAssignOpExpr(expr, arrAccess, label, null, op));
      }
      const assignOpTypes = [
        21 /* PlusAssign */,
        22 /* MinusAssign */,
        23 /* MultiplyAssign */,
        28 /* DivideAssign */,
        27 /* ModulusAssign */,
        25 /* AndAssign */,
        24 /* OrAssign */,
        26 /* XorAssign */,
        29 /* ShiftLeftAssign */,
        30 /* ShiftRightAssign */
      ];
      if (assignOpTypes.includes(this.peek().type)) {
        const op = this.advance();
        const rexpr = this.expression();
        return new SlotAssignOpExpr(expr, arrAccess, label, rexpr, op);
      }
      if (this.match(59 /* LParen */)) {
        const args2 = [expr];
        if (!this.check(60 /* RParen */)) {
          args2.push(this.expression());
          while (this.match(63 /* Comma */)) {
            args2.push(this.expression());
          }
        }
        this.consume(60 /* RParen */, "Expected ')' after arguments");
        return new FuncCallExpr(label, null, args2, 1 /* MethodCall */);
      }
      return expr;
    }
    if (expr instanceof VarExpr) {
      if (this.match(67 /* LeftSquareBracket */)) {
        const arrExpr = this.parseArrayIndex();
        this.consume(68 /* RightSquareBracket */, "Expected ']' after array index");
        expr.arrayIndex = arrExpr;
      }
      if (this.match(20 /* Assign */)) {
        const rexpr = this.expression();
        return new AssignExpr(expr, rexpr);
      }
      if (this.check(73 /* PlusPlus */) || this.check(74 /* MinusMinus */)) {
        const op = this.advance();
        return new ParenthesisExpr(new AssignOpExpr(expr, null, op));
      }
      const assignOpTypes = [
        21 /* PlusAssign */,
        22 /* MinusAssign */,
        23 /* MultiplyAssign */,
        28 /* DivideAssign */,
        27 /* ModulusAssign */,
        25 /* AndAssign */,
        24 /* OrAssign */,
        26 /* XorAssign */,
        29 /* ShiftLeftAssign */,
        30 /* ShiftRightAssign */
      ];
      if (assignOpTypes.includes(this.peek().type)) {
        const op = this.advance();
        const rexpr = this.expression();
        return new AssignOpExpr(expr, rexpr, op);
      }
      return expr;
    }
    if (expr instanceof SlotAccessExpr && expr.slotName !== null) {
      if (this.match(20 /* Assign */)) {
        const rexpr = this.expression();
        return new SlotAssignExpr(expr.objectExpr, null, expr.slotName, rexpr);
      }
      if (this.check(73 /* PlusPlus */) || this.check(74 /* MinusMinus */)) {
        const op = this.advance();
        return new ParenthesisExpr(new SlotAssignOpExpr(expr.objectExpr, null, expr.slotName, null, op));
      }
      const slotAssignOpTypes = [
        21 /* PlusAssign */,
        22 /* MinusAssign */,
        23 /* MultiplyAssign */,
        28 /* DivideAssign */,
        27 /* ModulusAssign */,
        25 /* AndAssign */,
        24 /* OrAssign */,
        26 /* XorAssign */,
        29 /* ShiftLeftAssign */,
        30 /* ShiftRightAssign */
      ];
      if (slotAssignOpTypes.includes(this.peek().type)) {
        const op = this.advance();
        const rexpr = this.expression();
        return new SlotAssignOpExpr(expr.objectExpr, null, expr.slotName, rexpr, op);
      }
      return expr;
    }
    if (expr instanceof SlotAccessExpr && expr.slotName === null && expr.arrayExpr !== null) {
      if (this.match(20 /* Assign */)) {
        const rexpr = this.expression();
        return new SlotAssignExpr(expr.objectExpr, expr.arrayExpr, null, rexpr);
      }
      if (this.check(73 /* PlusPlus */) || this.check(74 /* MinusMinus */)) {
        const op = this.advance();
        return new ParenthesisExpr(new SlotAssignOpExpr(expr.objectExpr, expr.arrayExpr, null, null, op));
      }
      return expr;
    }
    if (expr instanceof ConstantExpr) {
      if (this.match(59 /* LParen */)) {
        const args2 = [];
        if (!this.check(60 /* RParen */)) {
          args2.push(this.expression());
          while (this.match(63 /* Comma */)) {
            args2.push(this.expression());
          }
        }
        this.consume(60 /* RParen */, "Expected ')' after arguments");
        return new FuncCallExpr(expr.name, null, args2, 0 /* FunctionCall */);
      }
    }
    return expr;
  }
  // Expression parsing with precedence climbing
  expression() {
    return this.ternary();
  }
  ternary() {
    let expr = this.orExpr();
    if (this.match(71 /* QuestionMark */)) {
      const trueExpr = this.expression();
      this.consume(65 /* Colon */, "Expected ':' in ternary");
      const falseExpr = this.expression();
      expr = new ConditionalExpr(expr, trueExpr, falseExpr);
    }
    return expr;
  }
  orExpr() {
    let expr = this.andExpr();
    while (this.match(47 /* LogicalOr */)) {
      const right = this.andExpr();
      expr = new IntBinaryExpr(expr, right, this.previous());
    }
    return expr;
  }
  andExpr() {
    let expr = this.bitwiseOr();
    while (this.match(46 /* LogicalAnd */)) {
      const right = this.bitwiseOr();
      expr = new IntBinaryExpr(expr, right, this.previous());
    }
    return expr;
  }
  bitwiseOr() {
    let expr = this.bitwiseXor();
    while (this.match(51 /* BitwiseOr */)) {
      const right = this.bitwiseXor();
      expr = new IntBinaryExpr(expr, right, this.previous());
    }
    return expr;
  }
  bitwiseXor() {
    let expr = this.bitwiseAnd();
    while (this.match(52 /* BitwiseXor */)) {
      const right = this.bitwiseAnd();
      expr = new IntBinaryExpr(expr, right, this.previous());
    }
    return expr;
  }
  bitwiseAnd() {
    let expr = this.equality();
    while (this.match(50 /* BitwiseAnd */)) {
      const right = this.equality();
      expr = new IntBinaryExpr(expr, right, this.previous());
    }
    return expr;
  }
  equality() {
    let expr = this.strEquality();
    if (this.match(37 /* Equal */) || this.match(36 /* NotEqual */)) {
      const op = this.previous();
      const right = this.strEquality();
      expr = new IntBinaryExpr(expr, right, op);
    }
    return expr;
  }
  strEquality() {
    let expr = this.relational();
    if (this.match(39 /* StringEquals */) || this.match(40 /* StringNotEquals */)) {
      const op = this.previous();
      const right = this.relational();
      expr = new StrEqExpr(expr, right, op);
    }
    return expr;
  }
  relational() {
    let expr = this.shift();
    const relationalTypes = [31 /* LessThan */, 32 /* GreaterThan */, 33 /* LessThanEqual */, 34 /* GreaterThanEqual */];
    while (relationalTypes.includes(this.peek().type)) {
      const op = this.advance();
      const right = this.shift();
      expr = new IntBinaryExpr(expr, right, op);
    }
    return expr;
  }
  shift() {
    let expr = this.term();
    while (this.check(48 /* LeftBitShift */) || this.check(49 /* RightBitShift */)) {
      const op = this.advance();
      const right = this.term();
      expr = new IntBinaryExpr(expr, right, op);
    }
    return expr;
  }
  term() {
    let expr = this.strcat();
    while (this.check(15 /* Plus */) || this.check(16 /* Minus */)) {
      const op = this.advance();
      const right = this.strcat();
      expr = new FloatBinaryExpr(expr, right, op);
    }
    return expr;
  }
  strcat() {
    let expr = this.factor();
    const concatTypes = [41 /* Concat */, 42 /* SpaceConcat */, 43 /* TabConcat */, 44 /* NewlineConcat */];
    while (concatTypes.includes(this.peek().type)) {
      const op = this.advance();
      const right = this.factor();
      expr = new StrCatExpr(expr, right, op);
    }
    return expr;
  }
  factor() {
    let expr = this.unary();
    while (this.check(17 /* Multiply */) || this.check(18 /* Divide */) || this.check(19 /* Modulus */)) {
      const op = this.advance();
      const right = this.unary();
      expr = new FloatBinaryExpr(expr, right, op);
    }
    return expr;
  }
  unary() {
    if (this.match(16 /* Minus */)) {
      const op = this.previous();
      const expr = this.unary();
      return new FloatUnaryExpr(expr, op);
    }
    if (this.match(35 /* Not */)) {
      const op = this.previous();
      const expr = this.unary();
      return new IntUnaryExpr(expr, op);
    }
    if (this.match(38 /* Tilde */)) {
      const op = this.previous();
      const expr = this.unary();
      return new IntUnaryExpr(expr, op);
    }
    return this.chainExpr();
  }
  chainExpr() {
    let expr = this.primary();
    if (!expr) return new StringConstExpr(0, "", false);
    if (expr instanceof ConstantExpr && this.check(59 /* LParen */)) {
      this.advance();
      const args2 = [];
      if (!this.check(60 /* RParen */)) {
        args2.push(this.expression());
        while (this.match(63 /* Comma */)) {
          args2.push(this.expression());
        }
      }
      this.consume(60 /* RParen */, "Expected ')' after arguments");
      expr = new FuncCallExpr(expr.name, null, args2, 0 /* FunctionCall */);
    }
    while (this.check(70 /* Dot */)) {
      this.advance();
      const label = this.consume(53 /* Label */, "Expected label after .");
      let arrAccess = null;
      if (this.match(67 /* LeftSquareBracket */)) {
        arrAccess = this.parseArrayIndex();
        this.consume(68 /* RightSquareBracket */, "Expected ']'");
      }
      expr = new SlotAccessExpr(expr, arrAccess, label);
    }
    while ((expr instanceof ConstantExpr || expr instanceof VarExpr) && this.check(66 /* DoubleColon */)) {
      this.advance();
      const name = this.consume(53 /* Label */, "Expected name after ::");
      if (this.check(59 /* LParen */)) {
        this.advance();
        const args2 = [];
        if (!this.check(60 /* RParen */)) {
          args2.push(this.expression());
          while (this.match(63 /* Comma */)) {
            args2.push(this.expression());
          }
        }
        this.consume(60 /* RParen */, "Expected ')' after arguments");
        expr = new FuncCallExpr(name, expr instanceof ConstantExpr ? expr.name : expr.name, args2, 1 /* MethodCall */);
        break;
      } else {
        if (expr instanceof VarExpr) {
          expr = new VarExpr(name, expr.name, null, expr.vtype);
        } else {
          expr = new ConstantExpr(name);
        }
      }
    }
    if (expr instanceof FuncCallExpr) return expr;
    while (this.check(67 /* LeftSquareBracket */)) {
      this.advance();
      const index = this.parseArrayIndex();
      this.consume(68 /* RightSquareBracket */, "Expected ']' after array index");
      if (expr instanceof VarExpr && expr.arrayIndex === null) {
        expr.arrayIndex = index;
      } else {
        expr = new SlotAccessExpr(expr, index, null);
      }
    }
    while (this.check(70 /* Dot */)) {
      this.advance();
      const label = this.consume(53 /* Label */, "Expected label after .");
      let arrAccess = null;
      if (this.match(67 /* LeftSquareBracket */)) {
        arrAccess = this.parseArrayIndex();
        this.consume(68 /* RightSquareBracket */, "Expected ']'");
      }
      expr = new SlotAccessExpr(expr, arrAccess, label);
    }
    if (expr instanceof SlotAccessExpr && expr.slotName !== null && this.check(59 /* LParen */)) {
      this.advance();
      const args2 = [];
      if (!this.check(60 /* RParen */)) {
        args2.push(this.expression());
        while (this.match(63 /* Comma */)) {
          args2.push(this.expression());
        }
      }
      this.consume(60 /* RParen */, "Expected ')' after arguments");
      expr = new FuncCallExpr(expr.slotName, null, args2, 1 /* MethodCall */, expr.objectExpr);
      return expr;
    }
    return expr;
  }
  primary() {
    if (this.match(59 /* LParen */)) {
      const expr = this.expression();
      this.consume(60 /* RParen */, "Expected ')'");
      return new ParenthesisExpr(expr);
    }
    if (this.match(57 /* String */)) {
      return new StringConstExpr(this.previous().line, this.previous().literal, false);
    }
    if (this.match(58 /* TaggedString */)) {
      return new StringConstExpr(this.previous().line, this.previous().literal, true);
    }
    if (this.match(54 /* Int */)) {
      return new IntExpr(this.previous().line, this.previous().literal);
    }
    if (this.match(56 /* Float */)) {
      return new FloatExpr(this.previous().line, this.previous().literal);
    }
    if (this.match(55 /* HexInt */)) {
      return new IntExpr(this.previous().line, parseInt(this.previous().literal, 16));
    }
    if (this.match(12 /* True */)) {
      return new IntExpr(this.previous().line, 1);
    }
    if (this.match(13 /* False */)) {
      return new IntExpr(this.previous().line, 0);
    }
    if (this.match(9 /* New */)) {
      const className = this.consume(53 /* Label */, "Expected class name after new");
      return this.objectDecl(className, false, false);
    }
    if (this.match(53 /* Label */)) {
      return new ConstantExpr(this.previous());
    }
    if (this.match(69 /* Dollar */) || this.match(19 /* Modulus */)) {
      const isGlobal = this.previous().type === 69 /* Dollar */;
      return this.parseVariableNameToken(isGlobal);
    }
    return null;
  }
  // Utility methods
  match(type) {
    if (this.check(type)) {
      this.advance();
      return true;
    }
    return false;
  }
  check(type) {
    if (this.isAtEnd()) return false;
    return this.peek().type === type;
  }
  advance() {
    if (!this.isAtEnd()) this.current++;
    return this.previous();
  }
  isAtEnd() {
    return this.peek().type === 75 /* Eof */;
  }
  peek() {
    return this.tokens[this.current];
  }
  previous() {
    return this.tokens[this.current - 1];
  }
  consume(type, message) {
    if (this.check(type)) return this.advance();
    throw new SyntaxError(message, this.peek());
  }
  // Optional consume — silently skip if token not present (for lenient T2 parsing)
  tryConsume(type) {
    if (this.check(type)) {
      this.advance();
      return true;
    }
    return false;
  }
  objectDecl(className, structDecl, consumeTrailingSemicolon) {
    let objectNameExpr = new ConstantExpr(className);
    let parentObject = null;
    const args2 = [];
    if (this.match(59 /* LParen */)) {
      if (!this.check(60 /* RParen */)) {
        objectNameExpr = this.expression();
        if (this.match(65 /* Colon */)) {
          parentObject = this.consume(53 /* Label */, "Expected parent object name after :");
        }
        while (this.match(63 /* Comma */)) {
          args2.push(this.expression());
        }
      }
      this.consume(60 /* RParen */, "Expected ')' after object arguments");
    }
    const slots = [];
    const subObjects = [];
    if (this.match(61 /* LBracket */)) {
      while (!this.check(62 /* RBracket */) && !this.isAtEnd()) {
        if (this.match(9 /* New */)) {
          const subClassName = this.consume(53 /* Label */, "Expected class name after new");
          subObjects.push(this.objectDecl(subClassName, false, true));
          continue;
        }
        const slot = this.slotAssign();
        if (slot) {
          slots.push(slot);
          continue;
        }
        throw new SyntaxError("Expected slot assignment or nested object", this.peek());
      }
      this.consume(62 /* RBracket */, "Expected '}' after object body");
    }
    if (consumeTrailingSemicolon) {
      this.tryConsume(64 /* Semicolon */);
    }
    return new ObjectDeclExpr(
      new ConstantExpr(className),
      parentObject,
      objectNameExpr,
      args2,
      slots,
      subObjects,
      structDecl
    );
  }
};

// src/opcodes.ts
function buildOps(record, invalid) {
  const values = {};
  const tags = {};
  for (const [tag, val] of Object.entries(record)) {
    if (val !== null && val !== invalid) {
      values[tag] = val;
      tags[val] = tag;
    }
  }
  return { values, tags, invalid };
}
var tge10Ops = buildOps({
  OP_FUNC_DECL: 0,
  OP_CREATE_OBJECT: 1,
  OP_ADD_OBJECT: 4,
  OP_END_OBJECT: 5,
  OP_JMPIFFNOT: 6,
  OP_JMPIFNOT: 7,
  OP_JMPIFF: 8,
  OP_JMPIF: 9,
  OP_JMPIFNOT_NP: 10,
  OP_JMPIF_NP: 11,
  OP_JMP: 12,
  OP_RETURN: 13,
  OP_CMPEQ: 14,
  OP_CMPGR: 15,
  OP_CMPGE: 16,
  OP_CMPLT: 17,
  OP_CMPLE: 18,
  OP_CMPNE: 19,
  OP_XOR: 20,
  OP_MOD: 21,
  OP_BITAND: 22,
  OP_BITOR: 23,
  OP_NOT: 24,
  OP_NOTF: 25,
  OP_ONESCOMPLEMENT: 26,
  OP_SHR: 27,
  OP_SHL: 28,
  OP_AND: 29,
  OP_OR: 30,
  OP_ADD: 31,
  OP_SUB: 32,
  OP_MUL: 33,
  OP_DIV: 34,
  OP_NEG: 35,
  OP_SETCURVAR: 36,
  OP_SETCURVAR_CREATE: 37,
  OP_SETCURVAR_ARRAY: 38,
  OP_SETCURVAR_ARRAY_CREATE: 39,
  OP_LOADVAR_UINT: 40,
  OP_LOADVAR_FLT: 41,
  OP_LOADVAR_STR: 42,
  OP_SAVEVAR_UINT: 43,
  OP_SAVEVAR_FLT: 44,
  OP_SAVEVAR_STR: 45,
  OP_SETCUROBJECT: 46,
  OP_SETCUROBJECT_NEW: 47,
  OP_SETCUROBJECT_INTERNAL: null,
  OP_SETCURFIELD: 48,
  OP_SETCURFIELD_ARRAY: 49,
  OP_LOADFIELD_UINT: 50,
  OP_LOADFIELD_FLT: 51,
  OP_LOADFIELD_STR: 52,
  OP_SAVEFIELD_UINT: 53,
  OP_SAVEFIELD_FLT: 54,
  OP_SAVEFIELD_STR: 55,
  OP_STR_TO_UINT: 56,
  OP_STR_TO_FLT: 57,
  OP_STR_TO_NONE: 58,
  OP_FLT_TO_UINT: 59,
  OP_FLT_TO_STR: 60,
  OP_FLT_TO_NONE: 61,
  OP_UINT_TO_FLT: 62,
  OP_UINT_TO_STR: 63,
  OP_UINT_TO_NONE: 64,
  OP_LOADIMMED_UINT: 65,
  OP_LOADIMMED_FLT: 66,
  OP_TAG_TO_STR: 67,
  OP_LOADIMMED_STR: 68,
  OP_LOADIMMED_IDENT: 69,
  OP_CALLFUNC_RESOLVE: 70,
  OP_CALLFUNC: 71,
  OP_ADVANCE_STR: 73,
  OP_ADVANCE_STR_APPENDCHAR: 74,
  OP_ADVANCE_STR_COMMA: 75,
  OP_ADVANCE_STR_NUL: 76,
  OP_REWIND_STR: 77,
  OP_TERMINATE_REWIND_STR: 78,
  OP_COMPARE_STR: 79,
  OP_PUSH: 80,
  OP_PUSH_FRAME: 81,
  OP_BREAK: 82,
  OP_UNIT_CONVERSION: null,
  OP_UNUSED1: 2,
  OP_UNUSED2: 3,
  OP_UNUSED3: 72
}, 83);
var tge14Ops = buildOps({
  OP_FUNC_DECL: 0,
  OP_CREATE_OBJECT: 1,
  OP_ADD_OBJECT: 2,
  OP_END_OBJECT: 3,
  OP_JMPIFFNOT: 4,
  OP_JMPIFNOT: 5,
  OP_JMPIFF: 6,
  OP_JMPIF: 7,
  OP_JMPIFNOT_NP: 8,
  OP_JMPIF_NP: 9,
  OP_JMP: 10,
  OP_RETURN: 11,
  OP_CMPEQ: 12,
  OP_CMPGR: 13,
  OP_CMPGE: 14,
  OP_CMPLT: 15,
  OP_CMPLE: 16,
  OP_CMPNE: 17,
  OP_XOR: 18,
  OP_MOD: 19,
  OP_BITAND: 20,
  OP_BITOR: 21,
  OP_NOT: 22,
  OP_NOTF: 23,
  OP_ONESCOMPLEMENT: 24,
  OP_SHR: 25,
  OP_SHL: 26,
  OP_AND: 27,
  OP_OR: 28,
  OP_ADD: 29,
  OP_SUB: 30,
  OP_MUL: 31,
  OP_DIV: 32,
  OP_NEG: 33,
  OP_SETCURVAR: 34,
  OP_SETCURVAR_CREATE: 35,
  OP_SETCURVAR_ARRAY: 36,
  OP_SETCURVAR_ARRAY_CREATE: 37,
  OP_LOADVAR_UINT: 38,
  OP_LOADVAR_FLT: 39,
  OP_LOADVAR_STR: 40,
  OP_SAVEVAR_UINT: 41,
  OP_SAVEVAR_FLT: 42,
  OP_SAVEVAR_STR: 43,
  OP_SETCUROBJECT: 44,
  OP_SETCUROBJECT_NEW: 45,
  OP_SETCURFIELD: 46,
  OP_SETCURFIELD_ARRAY: 47,
  OP_LOADFIELD_UINT: 48,
  OP_LOADFIELD_FLT: 49,
  OP_LOADFIELD_STR: 50,
  OP_SAVEFIELD_UINT: 51,
  OP_SAVEFIELD_FLT: 52,
  OP_SAVEFIELD_STR: 53,
  OP_STR_TO_UINT: 54,
  OP_STR_TO_FLT: 55,
  OP_STR_TO_NONE: 56,
  OP_FLT_TO_UINT: 57,
  OP_FLT_TO_STR: 58,
  OP_FLT_TO_NONE: 59,
  OP_UINT_TO_FLT: 60,
  OP_UINT_TO_STR: 61,
  OP_UINT_TO_NONE: 62,
  OP_LOADIMMED_UINT: 63,
  OP_LOADIMMED_FLT: 64,
  OP_TAG_TO_STR: 65,
  OP_LOADIMMED_STR: 66,
  OP_LOADIMMED_IDENT: 67,
  OP_CALLFUNC_RESOLVE: 68,
  OP_CALLFUNC: 69,
  OP_ADVANCE_STR: 70,
  OP_ADVANCE_STR_APPENDCHAR: 71,
  OP_ADVANCE_STR_COMMA: 72,
  OP_ADVANCE_STR_NUL: 73,
  OP_REWIND_STR: 74,
  OP_TERMINATE_REWIND_STR: 75,
  OP_COMPARE_STR: 76,
  OP_PUSH: 77,
  OP_PUSH_FRAME: 78,
  OP_BREAK: 79,
  OP_UNUSED1: null,
  OP_UNUSED2: null,
  OP_UNUSED3: null
}, 80);
var constructorOps = buildOps({
  OP_FUNC_DECL: 0,
  OP_CREATE_OBJECT: 1,
  OP_ADD_OBJECT: 2,
  OP_END_OBJECT: 3,
  OP_JMPIFFNOT: 4,
  OP_JMPIFNOT: 5,
  OP_JMPIFF: 6,
  OP_JMPIF: 7,
  OP_JMPIFNOT_NP: 8,
  OP_JMPIF_NP: 9,
  OP_JMP: 10,
  OP_RETURN: 11,
  OP_CMPEQ: 12,
  OP_CMPGR: 13,
  OP_CMPGE: 14,
  OP_CMPLT: 15,
  OP_CMPLE: 16,
  OP_CMPNE: 17,
  OP_XOR: 18,
  OP_MOD: 19,
  OP_BITAND: 20,
  OP_BITOR: 21,
  OP_NOT: 22,
  OP_NOTF: 23,
  OP_ONESCOMPLEMENT: 24,
  OP_SHR: 25,
  OP_SHL: 26,
  OP_AND: 27,
  OP_OR: 28,
  OP_ADD: 29,
  OP_SUB: 30,
  OP_MUL: 31,
  OP_DIV: 32,
  OP_NEG: 33,
  OP_SETCURVAR: 34,
  OP_SETCURVAR_CREATE: 35,
  OP_SETCURVAR_ARRAY: 36,
  OP_SETCURVAR_ARRAY_CREATE: 37,
  OP_LOADVAR_UINT: 38,
  OP_LOADVAR_FLT: 39,
  OP_LOADVAR_STR: 40,
  OP_SAVEVAR_UINT: 41,
  OP_SAVEVAR_FLT: 42,
  OP_SAVEVAR_STR: 43,
  OP_SETCUROBJECT: 44,
  OP_SETCUROBJECT_NEW: 45,
  OP_SETCUROBJECT_INTERNAL: 80,
  OP_SETCURFIELD: 46,
  OP_SETCURFIELD_ARRAY: 47,
  OP_LOADFIELD_UINT: 48,
  OP_LOADFIELD_FLT: 49,
  OP_LOADFIELD_STR: 50,
  OP_SAVEFIELD_UINT: 51,
  OP_SAVEFIELD_FLT: 52,
  OP_SAVEFIELD_STR: 53,
  OP_STR_TO_UINT: 54,
  OP_STR_TO_FLT: 55,
  OP_STR_TO_NONE: 56,
  OP_FLT_TO_UINT: 57,
  OP_FLT_TO_STR: 58,
  OP_FLT_TO_NONE: 59,
  OP_UINT_TO_FLT: 60,
  OP_UINT_TO_STR: 61,
  OP_UINT_TO_NONE: 62,
  OP_LOADIMMED_UINT: 63,
  OP_LOADIMMED_FLT: 64,
  OP_TAG_TO_STR: 65,
  OP_LOADIMMED_STR: 66,
  OP_LOADIMMED_IDENT: 67,
  OP_CALLFUNC_RESOLVE: 68,
  OP_CALLFUNC: 69,
  OP_ADVANCE_STR: 70,
  OP_ADVANCE_STR_APPENDCHAR: 71,
  OP_ADVANCE_STR_COMMA: 72,
  OP_ADVANCE_STR_NUL: 73,
  OP_REWIND_STR: 74,
  OP_TERMINATE_REWIND_STR: 75,
  OP_COMPARE_STR: 76,
  OP_PUSH: 77,
  OP_PUSH_FRAME: 78,
  OP_BREAK: 79,
  OP_UNIT_CONVERSION: 81,
  OP_UNUSED1: null,
  OP_UNUSED2: null,
  OP_UNUSED3: null
}, 82);
var tfdOps = buildOps({
  OP_FUNC_DECL: 0,
  OP_CREATE_OBJECT: 1,
  OP_ADD_OBJECT: 4,
  OP_END_OBJECT: 5,
  OP_JMPIFFNOT: 6,
  OP_JMPIFNOT: 7,
  OP_JMPIFF: 8,
  OP_JMPIF: 9,
  OP_JMPIFNOT_NP: 10,
  OP_JMPIF_NP: 11,
  OP_JMP: 12,
  OP_RETURN: 13,
  OP_CMPEQ: 14,
  OP_CMPGR: 15,
  OP_CMPGE: 16,
  OP_CMPLT: 17,
  OP_CMPLE: 18,
  OP_CMPNE: 19,
  OP_XOR: 20,
  OP_MOD: 21,
  OP_BITAND: 22,
  OP_BITOR: 23,
  OP_NOT: 24,
  OP_NOTF: 25,
  OP_ONESCOMPLEMENT: 26,
  OP_SHR: 27,
  OP_SHL: 28,
  OP_AND: 29,
  OP_OR: 30,
  OP_ADD: 31,
  OP_SUB: 32,
  OP_MUL: 33,
  OP_DIV: 34,
  OP_NEG: 35,
  OP_SETCURVAR: 36,
  OP_SETCURVAR_CREATE: 37,
  OP_SETCURVAR_ARRAY: 38,
  OP_SETCURVAR_ARRAY_CREATE: 39,
  OP_LOADVAR_UINT: 40,
  OP_LOADVAR_FLT: 41,
  OP_LOADVAR_STR: 42,
  OP_SAVEVAR_UINT: 43,
  OP_SAVEVAR_FLT: 44,
  OP_SAVEVAR_STR: 45,
  OP_SETCUROBJECT: 46,
  OP_SETCUROBJECT_NEW: 47,
  OP_SETCUROBJECT_INTERNAL: 85,
  OP_SETCURFIELD: 48,
  OP_SETCURFIELD_ARRAY: 49,
  OP_LOADFIELD_UINT: 50,
  OP_LOADFIELD_FLT: 51,
  OP_LOADFIELD_STR: 52,
  OP_SAVEFIELD_UINT: 53,
  OP_SAVEFIELD_FLT: 54,
  OP_SAVEFIELD_STR: 55,
  OP_STR_TO_UINT: 56,
  OP_STR_TO_FLT: 57,
  OP_STR_TO_NONE: 58,
  OP_FLT_TO_UINT: 59,
  OP_FLT_TO_STR: 60,
  OP_FLT_TO_NONE: 61,
  OP_UINT_TO_FLT: 62,
  OP_UINT_TO_STR: 63,
  OP_UINT_TO_NONE: 64,
  OP_LOADIMMED_UINT: 65,
  OP_LOADIMMED_FLT: 66,
  OP_TAG_TO_STR: 67,
  OP_LOADIMMED_STR: 68,
  OP_LOADIMMED_IDENT: 69,
  OP_CALLFUNC_RESOLVE: 70,
  OP_CALLFUNC: 71,
  OP_ADVANCE_STR: 73,
  OP_ADVANCE_STR_APPENDCHAR: 74,
  OP_ADVANCE_STR_COMMA: 75,
  OP_ADVANCE_STR_NUL: 76,
  OP_REWIND_STR: 77,
  OP_TERMINATE_REWIND_STR: 78,
  OP_COMPARE_STR: 79,
  OP_PUSH: 80,
  OP_PUSH_FRAME: 81,
  OP_BREAK: 82,
  OP_UNIT_CONVERSION: 84,
  OP_UNUSED1: 2,
  OP_UNUSED2: 3,
  OP_UNUSED3: 72
}, 83);
var blv1Ops = buildOps({
  OP_FUNC_DECL: 0,
  OP_CREATE_OBJECT: 1,
  OP_ADD_OBJECT: 4,
  OP_END_OBJECT: 5,
  OP_JMPIFFNOT: 6,
  OP_JMPIFNOT: 7,
  OP_JMPIFF: 8,
  OP_JMPIF: 9,
  OP_JMPIFNOT_NP: 10,
  OP_JMPIF_NP: 11,
  OP_JMP: 12,
  OP_RETURN: 13,
  OP_CMPEQ: 14,
  OP_CMPGR: 15,
  OP_CMPGE: 16,
  OP_CMPLT: 17,
  OP_CMPLE: 18,
  OP_CMPNE: 19,
  OP_XOR: 20,
  OP_MOD: 21,
  OP_BITAND: 22,
  OP_BITOR: 23,
  OP_NOT: 24,
  OP_NOTF: 25,
  OP_ONESCOMPLEMENT: 26,
  OP_SHR: 27,
  OP_SHL: 28,
  OP_AND: 29,
  OP_OR: 30,
  OP_ADD: 31,
  OP_SUB: 32,
  OP_MUL: 33,
  OP_DIV: 34,
  OP_NEG: 35,
  OP_SETCURVAR: 36,
  OP_SETCURVAR_CREATE: 37,
  OP_SETCURVAR_ARRAY: 38,
  OP_SETCURVAR_ARRAY_CREATE: 39,
  OP_LOADVAR_UINT: 40,
  OP_LOADVAR_FLT: 41,
  OP_LOADVAR_STR: 42,
  OP_SAVEVAR_UINT: 43,
  OP_SAVEVAR_FLT: 44,
  OP_SAVEVAR_STR: 45,
  OP_SETCUROBJECT: 46,
  OP_SETCUROBJECT_NEW: 47,
  OP_SETCUROBJECT_INTERNAL: 85,
  OP_SETCURFIELD: 48,
  OP_SETCURFIELD_ARRAY: 49,
  OP_LOADFIELD_UINT: 50,
  OP_LOADFIELD_FLT: 51,
  OP_LOADFIELD_STR: 52,
  OP_SAVEFIELD_UINT: 53,
  OP_SAVEFIELD_FLT: 54,
  OP_SAVEFIELD_STR: 55,
  OP_STR_TO_UINT: 56,
  OP_STR_TO_FLT: 57,
  OP_STR_TO_NONE: 58,
  OP_FLT_TO_UINT: 59,
  OP_FLT_TO_STR: 60,
  OP_FLT_TO_NONE: 61,
  OP_UINT_TO_FLT: 62,
  OP_UINT_TO_STR: 63,
  OP_UINT_TO_NONE: 64,
  OP_LOADIMMED_UINT: 65,
  OP_LOADIMMED_FLT: 66,
  OP_TAG_TO_STR: 67,
  OP_LOADIMMED_STR: 68,
  OP_LOADIMMED_IDENT: 69,
  OP_CALLFUNC_RESOLVE: 70,
  OP_CALLFUNC: 71,
  OP_ADVANCE_STR: 73,
  OP_ADVANCE_STR_APPENDCHAR: 74,
  OP_ADVANCE_STR_COMMA: 75,
  OP_ADVANCE_STR_NUL: 76,
  OP_REWIND_STR: 77,
  OP_TERMINATE_REWIND_STR: 78,
  OP_COMPARE_STR: 79,
  OP_PUSH: 80,
  OP_PUSH_FRAME: 81,
  OP_BREAK: 82,
  OP_UNIT_CONVERSION: 84,
  OP_UNUSED1: 2,
  OP_UNUSED2: 3,
  OP_UNUSED3: 72
}, 83);
var blv20Ops = buildOps({
  OP_FUNC_DECL: 83,
  OP_CREATE_OBJECT: 82,
  OP_ADD_OBJECT: 9,
  OP_END_OBJECT: 59,
  OP_JMPIFFNOT: 26,
  OP_JMPIFNOT: 43,
  OP_JMPIFF: 42,
  OP_JMPIF: 17,
  OP_JMPIFNOT_NP: 70,
  OP_JMPIF_NP: 25,
  OP_JMP: 27,
  OP_RETURN: 72,
  OP_CMPEQ: 49,
  OP_CMPGR: 50,
  OP_CMPGE: 74,
  OP_CMPLT: 60,
  OP_CMPLE: 22,
  OP_CMPNE: 51,
  OP_XOR: 73,
  OP_MOD: 40,
  OP_BITAND: 77,
  OP_BITOR: 29,
  OP_NOT: 64,
  OP_NOTF: 65,
  OP_ONESCOMPLEMENT: 78,
  OP_SHR: 31,
  OP_SHL: 30,
  OP_AND: 71,
  OP_OR: 52,
  OP_ADD: 68,
  OP_SUB: 45,
  OP_MUL: 46,
  OP_DIV: 47,
  OP_NEG: 48,
  OP_SETCURVAR: 66,
  OP_SETCURVAR_CREATE: 62,
  OP_SETCURVAR_ARRAY: 67,
  OP_SETCURVAR_ARRAY_CREATE: 69,
  OP_LOADVAR_UINT: 15,
  OP_LOADVAR_FLT: 1,
  OP_LOADVAR_STR: 0,
  OP_SAVEVAR_UINT: 44,
  OP_SAVEVAR_FLT: 18,
  OP_SAVEVAR_STR: 16,
  OP_SETCUROBJECT: 35,
  OP_SETCUROBJECT_NEW: 63,
  OP_SETCURFIELD: 23,
  OP_SETCURFIELD_ARRAY: 24,
  OP_LOADFIELD_UINT: 41,
  OP_LOADFIELD_FLT: 3,
  OP_LOADFIELD_STR: 75,
  OP_SAVEFIELD_UINT: 38,
  OP_SAVEFIELD_FLT: 2,
  OP_SAVEFIELD_STR: 76,
  OP_STR_TO_UINT: 34,
  OP_STR_TO_FLT: 58,
  OP_STR_TO_NONE: 32,
  OP_FLT_TO_UINT: 12,
  OP_FLT_TO_STR: 13,
  OP_FLT_TO_NONE: 81,
  OP_UINT_TO_FLT: 4,
  OP_UINT_TO_STR: 6,
  OP_UINT_TO_NONE: 7,
  OP_LOADIMMED_UINT: 53,
  OP_LOADIMMED_FLT: 54,
  OP_TAG_TO_STR: 56,
  OP_LOADIMMED_STR: 57,
  OP_LOADIMMED_IDENT: 55,
  OP_CALLFUNC_RESOLVE: 11,
  OP_CALLFUNC: 39,
  OP_ADVANCE_STR: 21,
  OP_ADVANCE_STR_APPENDCHAR: 19,
  OP_ADVANCE_STR_COMMA: 79,
  OP_ADVANCE_STR_NUL: 5,
  OP_REWIND_STR: 37,
  OP_TERMINATE_REWIND_STR: 20,
  OP_COMPARE_STR: 33,
  OP_PUSH: 80,
  OP_PUSH_FRAME: 36,
  OP_BREAK: 61,
  OP_UNUSED1: 8,
  OP_UNUSED2: 10,
  OP_UNUSED3: 14
}, 28);
var blv21Ops = buildOps({
  OP_FUNC_DECL: 83,
  OP_CREATE_OBJECT: 82,
  OP_ADD_OBJECT: 5,
  OP_END_OBJECT: 59,
  OP_JMPIFFNOT: 50,
  OP_JMPIFNOT: 13,
  OP_JMPIFF: 27,
  OP_JMPIF: 51,
  OP_JMPIFNOT_NP: 70,
  OP_JMPIF_NP: 26,
  OP_JMP: 28,
  OP_RETURN: 72,
  OP_CMPEQ: 34,
  OP_CMPGR: 35,
  OP_CMPGE: 74,
  OP_CMPLT: 60,
  OP_CMPLE: 23,
  OP_CMPNE: 36,
  OP_XOR: 73,
  OP_MOD: 48,
  OP_BITAND: 77,
  OP_BITOR: 29,
  OP_NOT: 64,
  OP_NOTF: 65,
  OP_ONESCOMPLEMENT: 78,
  OP_SHR: 31,
  OP_SHL: 30,
  OP_AND: 71,
  OP_OR: 37,
  OP_ADD: 68,
  OP_SUB: 53,
  OP_MUL: 54,
  OP_DIV: 55,
  OP_NEG: 56,
  OP_SETCURVAR: 66,
  OP_SETCURVAR_CREATE: 62,
  OP_SETCURVAR_ARRAY: 67,
  OP_SETCURVAR_ARRAY_CREATE: 69,
  OP_LOADVAR_UINT: 11,
  OP_LOADVAR_FLT: 45,
  OP_LOADVAR_STR: 44,
  OP_SAVEVAR_UINT: 52,
  OP_SAVEVAR_FLT: 14,
  OP_SAVEVAR_STR: 12,
  OP_SETCUROBJECT: 39,
  OP_SETCUROBJECT_NEW: 63,
  OP_SETCURFIELD: 24,
  OP_SETCURFIELD_ARRAY: 25,
  OP_LOADFIELD_UINT: 49,
  OP_LOADFIELD_FLT: 47,
  OP_LOADFIELD_STR: 75,
  OP_SAVEFIELD_UINT: 42,
  OP_SAVEFIELD_FLT: 46,
  OP_SAVEFIELD_STR: 76,
  OP_STR_TO_UINT: 38,
  OP_STR_TO_FLT: 58,
  OP_STR_TO_NONE: 32,
  OP_FLT_TO_UINT: 8,
  OP_FLT_TO_STR: 9,
  OP_FLT_TO_NONE: 81,
  OP_UINT_TO_FLT: 0,
  OP_UINT_TO_STR: 2,
  OP_UINT_TO_NONE: 3,
  OP_LOADIMMED_UINT: 15,
  OP_LOADIMMED_FLT: 16,
  OP_TAG_TO_STR: 18,
  OP_LOADIMMED_STR: 19,
  OP_LOADIMMED_IDENT: 17,
  OP_CALLFUNC_RESOLVE: 7,
  OP_CALLFUNC: 43,
  OP_ADVANCE_STR: 22,
  OP_ADVANCE_STR_APPENDCHAR: 20,
  OP_ADVANCE_STR_COMMA: 79,
  OP_ADVANCE_STR_NUL: 1,
  OP_REWIND_STR: 41,
  OP_TERMINATE_REWIND_STR: 21,
  OP_COMPARE_STR: 33,
  OP_PUSH: 80,
  OP_PUSH_FRAME: 40,
  OP_BREAK: 61,
  OP_UNUSED1: 4,
  OP_UNUSED2: 6,
  OP_UNUSED3: 10
}, 57);
var OPS_MAPS = {
  TGE10: tge10Ops,
  Tribes2: tge10Ops,
  TGE14: tge14Ops,
  Constructor: constructorOps,
  TFD: tfdOps,
  BlocklandV1: blv1Ops,
  BlocklandV20: blv20Ops,
  BlocklandV21: blv21Ops
};

// src/compiler/compiler-types.ts
var StringTable = class {
  constructor() {
    this.entries = [];
    this.stringToIndex = /* @__PURE__ */ new Map();
    this.totalLen = 0;
  }
  add(str, caseSens, tag) {
    const key = caseSens ? str : str.toLowerCase();
    if (this.stringToIndex.has(key)) return this.stringToIndex.get(key);
    const len = tag && str.length + 1 < 7 ? 7 : str.length + 1;
    const entry = { string: str, start: this.totalLen, len, tag };
    this.entries.push(entry);
    this.totalLen += len;
    this.stringToIndex.set(key, entry.start);
    return entry.start;
  }
};
var IdentTable = class {
  constructor() {
    this.identMap = /* @__PURE__ */ new Map();
    this.ipToIdentMap = /* @__PURE__ */ new Map();
  }
  add(strTable, ident, ip) {
    const targetTable = this.globalStringTable ?? strTable;
    const index = targetTable.add(ident, false, false);
    if (this.identMap.has(index)) {
      this.identMap.get(index).push(ip);
    } else {
      this.identMap.set(index, [ip]);
    }
    this.ipToIdentMap.set(ip, ident);
  }
  reset() {
    this.identMap.clear();
    this.ipToIdentMap.clear();
  }
};
var CompileContext = class {
  constructor(codeSize, lineBreakPairSize) {
    this.ip = 0;
    this.continuePoint = 0;
    this.breakPoint = 0;
    this.codeStream = new Array(codeSize).fill(0);
    this.lineBreakPairs = new Array(lineBreakPairSize).fill(0);
  }
};

// src/compiler/compiler.ts
var opcodeNameMap = {};
opcodeNameMap[0 /* FuncDecl */] = "OP_FUNC_DECL";
opcodeNameMap[1 /* CreateObject */] = "OP_CREATE_OBJECT";
opcodeNameMap[4 /* AddObject */] = "OP_ADD_OBJECT";
opcodeNameMap[5 /* EndObject */] = "OP_END_OBJECT";
opcodeNameMap[6 /* JmpIffNot */] = "OP_JMPIFFNOT";
opcodeNameMap[7 /* JmpIfNot */] = "OP_JMPIFNOT";
opcodeNameMap[8 /* JmpIff */] = "OP_JMPIFF";
opcodeNameMap[9 /* JmpIf */] = "OP_JMPIF";
opcodeNameMap[10 /* JmpIfNotNP */] = "OP_JMPIFNOT_NP";
opcodeNameMap[11 /* JmpIfNP */] = "OP_JMPIF_NP";
opcodeNameMap[12 /* Jmp */] = "OP_JMP";
opcodeNameMap[13 /* Return */] = "OP_RETURN";
opcodeNameMap[14 /* CmpEQ */] = "OP_CMPEQ";
opcodeNameMap[15 /* CmpGT */] = "OP_CMPGR";
opcodeNameMap[16 /* CmpGE */] = "OP_CMPGE";
opcodeNameMap[17 /* CmpLT */] = "OP_CMPLT";
opcodeNameMap[18 /* CmpLE */] = "OP_CMPLE";
opcodeNameMap[19 /* CmpNE */] = "OP_CMPNE";
opcodeNameMap[20 /* Xor */] = "OP_XOR";
opcodeNameMap[21 /* Mod */] = "OP_MOD";
opcodeNameMap[22 /* BitAnd */] = "OP_BITAND";
opcodeNameMap[23 /* BitOr */] = "OP_BITOR";
opcodeNameMap[24 /* Not */] = "OP_NOT";
opcodeNameMap[25 /* NotF */] = "OP_NOTF";
opcodeNameMap[26 /* OnesComplement */] = "OP_ONESCOMPLEMENT";
opcodeNameMap[27 /* Shr */] = "OP_SHR";
opcodeNameMap[28 /* Shl */] = "OP_SHL";
opcodeNameMap[29 /* And */] = "OP_AND";
opcodeNameMap[30 /* Or */] = "OP_OR";
opcodeNameMap[31 /* Add */] = "OP_ADD";
opcodeNameMap[32 /* Sub */] = "OP_SUB";
opcodeNameMap[33 /* Mul */] = "OP_MUL";
opcodeNameMap[34 /* Div */] = "OP_DIV";
opcodeNameMap[35 /* Neg */] = "OP_NEG";
opcodeNameMap[36 /* SetCurVar */] = "OP_SETCURVAR";
opcodeNameMap[37 /* SetCurVarCreate */] = "OP_SETCURVAR_CREATE";
opcodeNameMap[38 /* SetCurVarArray */] = "OP_SETCURVAR_ARRAY";
opcodeNameMap[39 /* SetCurVarArrayCreate */] = "OP_SETCURVAR_ARRAY_CREATE";
opcodeNameMap[40 /* LoadVarUInt */] = "OP_LOADVAR_UINT";
opcodeNameMap[41 /* LoadVarFlt */] = "OP_LOADVAR_FLT";
opcodeNameMap[42 /* LoadVarStr */] = "OP_LOADVAR_STR";
opcodeNameMap[43 /* SaveVarUInt */] = "OP_SAVEVAR_UINT";
opcodeNameMap[44 /* SaveVarFlt */] = "OP_SAVEVAR_FLT";
opcodeNameMap[45 /* SaveVarStr */] = "OP_SAVEVAR_STR";
opcodeNameMap[46 /* SetCurObject */] = "OP_SETCUROBJECT";
opcodeNameMap[47 /* SetCurObjectNew */] = "OP_SETCUROBJECT_NEW";
opcodeNameMap[48 /* SetCurField */] = "OP_SETCURFIELD";
opcodeNameMap[49 /* SetCurFieldArray */] = "OP_SETCURFIELD_ARRAY";
opcodeNameMap[50 /* LoadFieldUInt */] = "OP_LOADFIELD_UINT";
opcodeNameMap[51 /* LoadFieldFlt */] = "OP_LOADFIELD_FLT";
opcodeNameMap[52 /* LoadFieldStr */] = "OP_LOADFIELD_STR";
opcodeNameMap[53 /* SaveFieldUInt */] = "OP_SAVEFIELD_UINT";
opcodeNameMap[54 /* SaveFieldFlt */] = "OP_SAVEFIELD_FLT";
opcodeNameMap[55 /* SaveFieldStr */] = "OP_SAVEFIELD_STR";
opcodeNameMap[56 /* StrToUInt */] = "OP_STR_TO_UINT";
opcodeNameMap[57 /* StrToFlt */] = "OP_STR_TO_FLT";
opcodeNameMap[58 /* StrToNone */] = "OP_STR_TO_NONE";
opcodeNameMap[59 /* FltToUInt */] = "OP_FLT_TO_UINT";
opcodeNameMap[60 /* FltToStr */] = "OP_FLT_TO_STR";
opcodeNameMap[61 /* FltToNone */] = "OP_FLT_TO_NONE";
opcodeNameMap[62 /* UIntToFlt */] = "OP_UINT_TO_FLT";
opcodeNameMap[63 /* UIntToStr */] = "OP_UINT_TO_STR";
opcodeNameMap[64 /* UIntToNone */] = "OP_UINT_TO_NONE";
opcodeNameMap[65 /* LoadImmedUInt */] = "OP_LOADIMMED_UINT";
opcodeNameMap[66 /* LoadImmedFlt */] = "OP_LOADIMMED_FLT";
opcodeNameMap[67 /* TagToStr */] = "OP_TAG_TO_STR";
opcodeNameMap[68 /* LoadImmedStr */] = "OP_LOADIMMED_STR";
opcodeNameMap[69 /* LoadImmedIdent */] = "OP_LOADIMMED_IDENT";
opcodeNameMap[70 /* CallFuncResolve */] = "OP_CALLFUNC_RESOLVE";
opcodeNameMap[71 /* CallFunc */] = "OP_CALLFUNC";
opcodeNameMap[73 /* AdvanceStr */] = "OP_ADVANCE_STR";
opcodeNameMap[74 /* AdvanceStrAppendChar */] = "OP_ADVANCE_STR_APPENDCHAR";
opcodeNameMap[75 /* AdvanceStrComma */] = "OP_ADVANCE_STR_COMMA";
opcodeNameMap[76 /* AdvanceStrNul */] = "OP_ADVANCE_STR_NUL";
opcodeNameMap[77 /* RewindStr */] = "OP_REWIND_STR";
opcodeNameMap[78 /* TerminateRewindStr */] = "OP_TERMINATE_REWIND_STR";
opcodeNameMap[79 /* CompareStr */] = "OP_COMPARE_STR";
opcodeNameMap[80 /* Push */] = "OP_PUSH";
opcodeNameMap[81 /* PushFrame */] = "OP_PUSH_FRAME";
opcodeNameMap[82 /* Break */] = "OP_BREAK";
var Compiler = class {
  constructor(targetId = "TGE10") {
    this.globalFloatTable = [];
    this.functionFloatTable = [];
    this.globalStringTable = new StringTable();
    this.functionStringTable = new StringTable();
    this.identTable = new IdentTable();
    this.inFunction = false;
    this.breakLineCount = 0;
    this.ops = OPS_MAPS[targetId] || OPS_MAPS["TGE10"];
    this.currentStringTable = this.globalStringTable;
    this.currentFloatTable = this.globalFloatTable;
    const versionMap = {
      TGE10: 33,
      Tribes2: 174,
      TGE14: 36,
      Constructor: 38,
      TFD: 33,
      BlocklandV1: 90,
      BlocklandV20: 190,
      BlocklandV21: 210
    };
    this.dsoVersion = versionMap[targetId] || 33;
  }
  getOpcodeValue(opCode) {
    const tagName = opcodeNameMap[opCode];
    if (!tagName || !(tagName in this.ops.values)) return this.ops.invalid;
    return this.ops.values[tagName];
  }
  compile(code2) {
    const scanner = new Scanner(code2);
    const tokens = scanner.scanTokens();
    const parser = new Parser(tokens);
    const ast = parser.parse();
    return this.compileAST(ast);
  }
  compileAST(stmts) {
    this.globalFloatTable = [];
    this.functionFloatTable = [];
    this.globalStringTable = new StringTable();
    this.functionStringTable = new StringTable();
    this.identTable = new IdentTable();
    this.identTable.globalStringTable = this.globalStringTable;
    this.currentStringTable = this.globalStringTable;
    this.currentFloatTable = this.globalFloatTable;
    this.inFunction = false;
    this.breakLineCount = 0;
    const codeSize = this.precompileBlock(stmts, 0) + 1;
    const breakCount = this.breakLineCount;
    const lineBreakPairCount = breakCount * 2;
    const context = new CompileContext(codeSize * 4 + 1024, lineBreakPairCount * 2 + 1024);
    this.breakLineCount = 0;
    if (stmts.length > 0) this.compileBlock(context, stmts);
    context.codeStream[context.ip++] = this.getOpcodeValue(13 /* Return */);
    return this.serialize(context, context.ip, this.breakLineCount * 2);
  }
  // --- Precompile pass ---
  precompileBlock(stmts, loopCount) {
    let sum = 0;
    for (const s of stmts) sum += this.precompileStmt(s, loopCount);
    return sum;
  }
  precompileStmt(stmt, loopCount) {
    if (stmt instanceof ReturnStmt) {
      this.breakLineCount++;
      return 1 + (stmt.expr ? this.precompileExpr(stmt.expr, 3 /* String */) : 0);
    }
    if (stmt instanceof BreakStmt) {
      if (loopCount > 0) {
        this.breakLineCount++;
        return 2;
      }
      return 0;
    }
    if (stmt instanceof ContinueStmt) {
      if (loopCount > 0) {
        this.breakLineCount++;
        return 2;
      }
      return 0;
    }
    if (stmt instanceof IfStmt) {
      this.breakLineCount++;
      const exprSize = this.precompileExpr(stmt.condition, 2 /* Float */);
      const ifSize = this.precompileBlock(stmt.body, loopCount);
      const elseSize = stmt.elseBody ? this.precompileBlock(stmt.elseBody, loopCount) : 0;
      return exprSize + 2 + ifSize + (stmt.elseBody ? 2 + elseSize : 0);
    }
    if (stmt instanceof LoopStmt) {
      this.breakLineCount++;
      const initSize = stmt.init ? this.precompileExpr(stmt.init, 0 /* None */) : 0;
      const testSize = this.precompileExpr(stmt.condition, 2 /* Float */);
      const blockSize = this.precompileBlock(stmt.body, loopCount + 1);
      const endSize = stmt.end ? this.precompileExpr(stmt.end, 0 /* None */) : 0;
      return initSize + testSize + 2 + blockSize + endSize + testSize + 2;
    }
    if (stmt instanceof FunctionDeclStmt) {
      this.breakLineCount++;
      return this.precompileFunction(stmt);
    }
    if (stmt instanceof PackageDeclStmt) {
      this.breakLineCount++;
      let sum = 0;
      for (const fn of stmt.functions) sum += this.precompileFunction(fn);
      return sum;
    }
    this.breakLineCount++;
    return this.precompileExpr(stmt, 0 /* None */);
  }
  precompileExpr(expr, typeReq) {
    if (expr instanceof IntExpr || expr instanceof FloatExpr || expr instanceof StringConstExpr || expr instanceof ConstantExpr) {
      return typeReq === 0 /* None */ ? 0 : 2;
    }
    if (expr instanceof FloatBinaryExpr) {
      return this.precompileExpr(expr.left, 2 /* Float */) + this.precompileExpr(expr.right, 2 /* Float */) + 1 + (typeReq !== 2 /* Float */ ? 1 : 0);
    }
    if (expr instanceof IntBinaryExpr) {
      return this.precompileExpr(expr.left, 1 /* Int */) + this.precompileExpr(expr.right, 1 /* Int */) + 1 + (typeReq !== 1 /* Int */ ? 1 : 0);
    }
    if (expr instanceof StrCatExpr) {
      return this.precompileExpr(expr.left, 3 /* String */) + this.precompileExpr(expr.right, 3 /* String */) + 2 + (typeReq !== 3 /* String */ ? 1 : 0);
    }
    if (expr instanceof StrEqExpr) {
      return this.precompileExpr(expr.left, 3 /* String */) + this.precompileExpr(expr.right, 3 /* String */) + 2 + (typeReq !== 1 /* Int */ ? 1 : 0);
    }
    if (expr instanceof VarExpr) {
      return typeReq === 0 /* None */ ? 0 : expr.arrayIndex ? 6 : 3;
    }
    if (expr instanceof AssignExpr) {
      return this.precompileExpr(expr.expr, 3 /* String */) + 3;
    }
    if (expr instanceof AssignOpExpr) {
      return this.precompileExpr(expr.expr, 2 /* Float */) + 5;
    }
    if (expr instanceof FuncCallExpr) {
      let size = 5;
      for (const arg of expr.args) size += this.precompileExpr(arg, 3 /* String */) + 1;
      return size;
    }
    if (expr instanceof SlotAccessExpr) {
      return this.precompileExpr(expr.objectExpr, 3 /* String */) + 3 + (expr.arrayExpr ? this.precompileExpr(expr.arrayExpr, 3 /* String */) + 3 : 0);
    }
    if (expr instanceof SlotAssignExpr) {
      return this.precompileExpr(expr.expr, 3 /* String */) + 5 + (expr.arrayExpr ? this.precompileExpr(expr.arrayExpr, 3 /* String */) + 3 : 0) + (expr.objectExpr ? this.precompileExpr(expr.objectExpr, 3 /* String */) : 0);
    }
    if (expr instanceof ObjectDeclExpr) return this.precompileObjectDecl(expr);
    if (expr instanceof ParenthesisExpr) return this.precompileExpr(expr.expr, typeReq);
    if (expr instanceof IntUnaryExpr) return this.precompileExpr(expr.expr, 1 /* Int */) + 1 + (typeReq !== 1 /* Int */ ? 1 : 0);
    if (expr instanceof FloatUnaryExpr) return this.precompileExpr(expr.expr, 2 /* Float */) + 1 + (typeReq !== 2 /* Float */ ? 1 : 0);
    if (expr instanceof ConditionalExpr) {
      return this.precompileExpr(expr.condition, 1 /* Int */) + this.precompileExpr(expr.trueExpr, typeReq) + this.precompileExpr(expr.falseExpr, typeReq) + 4;
    }
    return 0;
  }
  precompileObjectDecl(expr) {
    let size = 10;
    size += this.precompileExpr(expr.className, 3 /* String */) + 1;
    size += this.precompileExpr(expr.objectNameExpr, 3 /* String */) + 1;
    for (const arg of expr.args) size += this.precompileExpr(arg, 3 /* String */) + 1;
    for (const slot of expr.slotDecls) size += this.precompileExpr(slot, 0 /* None */);
    for (const sub of expr.subObjects) size += this.precompileObjectDecl(sub);
    return size;
  }
  precompileFunction(fn) {
    const prevS = this.currentStringTable, prevF = this.currentFloatTable;
    this.currentStringTable = this.functionStringTable;
    this.currentFloatTable = this.functionFloatTable;
    const bodySize = this.precompileBlock(fn.stmts, 0);
    this.currentStringTable = prevS;
    this.currentFloatTable = prevF;
    return 7 + fn.args.length + bodySize;
  }
  // --- Compile pass ---
  compileBlock(context, stmts) {
    for (const stmt of stmts) this.compileStmt(context, stmt);
  }
  compileStmt(context, stmt) {
    this.addBreakLine(context, stmt.lineNo);
    if (stmt instanceof ReturnStmt) {
      if (stmt.expr) this.compileExpr(context, stmt.expr, 3 /* String */);
      this.emit(context, this.getOpcodeValue(13 /* Return */));
    } else if (stmt instanceof BreakStmt) {
      if (context.breakPoint > 0) this.emit(context, this.getOpcodeValue(12 /* Jmp */), context.breakPoint);
    } else if (stmt instanceof ContinueStmt) {
      if (context.continuePoint > 0) this.emit(context, this.getOpcodeValue(12 /* Jmp */), context.continuePoint);
    } else if (stmt instanceof IfStmt) {
      this.compileIf(context, stmt);
    } else if (stmt instanceof LoopStmt) {
      this.compileLoop(context, stmt);
    } else if (stmt instanceof FunctionDeclStmt) {
      this.compileFunction(context, stmt);
    } else if (stmt instanceof PackageDeclStmt) {
      this.compilePackage(context, stmt);
    } else if (stmt instanceof Expr) {
      this.compileExpr(context, stmt, 0 /* None */);
    }
  }
  compileIf(context, stmt) {
    this.compileExpr(context, stmt.condition, 2 /* Float */);
    this.emit(context, this.getOpcodeValue(6 /* JmpIffNot */));
    const jmpIp = context.ip++;
    this.compileBlock(context, stmt.body);
    if (stmt.elseBody) {
      this.emit(context, this.getOpcodeValue(12 /* Jmp */));
      const endJmpIp = context.ip++;
      context.codeStream[jmpIp] = context.ip;
      this.compileBlock(context, stmt.elseBody);
      context.codeStream[endJmpIp] = context.ip;
    } else {
      context.codeStream[jmpIp] = context.ip;
    }
  }
  compileLoop(context, stmt) {
    if (stmt.init) {
      this.compileExpr(context, stmt.condition, 2 /* Float */);
      this.emit(context, this.getOpcodeValue(6 /* JmpIffNot */));
      const breakJmpIp = context.ip++;
      this.compileExpr(context, stmt.init, 0 /* None */);
      const bodyStart = context.ip;
      const savedBreak = context.breakPoint, savedCont = context.continuePoint;
      context.breakPoint = 0;
      context.continuePoint = 0;
      this.compileBlock(context, stmt.body);
      context.continuePoint = context.ip;
      if (stmt.end) this.compileExpr(context, stmt.end, 0 /* None */);
      this.compileExpr(context, stmt.condition, 2 /* Float */);
      this.emit(context, this.getOpcodeValue(8 /* JmpIff */), bodyStart);
      context.codeStream[breakJmpIp] = context.ip;
      context.breakPoint = savedBreak;
      context.continuePoint = savedCont;
    } else {
      this.compileExpr(context, stmt.condition, 2 /* Float */);
      this.emit(context, this.getOpcodeValue(6 /* JmpIffNot */));
      const breakJmpIp = context.ip++;
      const bodyStart = context.ip;
      const savedBreak = context.breakPoint, savedCont = context.continuePoint;
      context.breakPoint = 0;
      context.continuePoint = 0;
      this.compileBlock(context, stmt.body);
      context.continuePoint = context.ip;
      this.compileExpr(context, stmt.condition, 2 /* Float */);
      this.emit(context, this.getOpcodeValue(8 /* JmpIff */), bodyStart);
      context.codeStream[breakJmpIp] = context.ip;
      context.breakPoint = savedBreak;
      context.continuePoint = savedCont;
    }
  }
  compileFunction(context, fn) {
    const prevS = this.currentStringTable, prevF = this.currentFloatTable;
    this.currentStringTable = this.functionStringTable;
    this.currentFloatTable = this.functionFloatTable;
    this.inFunction = true;
    const start = context.ip;
    this.emit(context, this.getOpcodeValue(0 /* FuncDecl */));
    const nameIdx = this.globalStringTable.add(fn.functionName.literal, false, false);
    context.codeStream[context.ip] = nameIdx;
    this.identTable.add(this.globalStringTable, fn.functionName.literal, context.ip);
    context.ip++;
    const nsIdx = fn.namespace ? this.globalStringTable.add(fn.namespace.literal, false, false) : 0;
    context.codeStream[context.ip] = nsIdx;
    if (fn.namespace) this.identTable.add(this.globalStringTable, fn.namespace.literal, context.ip);
    context.ip++;
    const pkgIdx = fn.packageName ? this.globalStringTable.add(fn.packageName.literal, false, false) : 0;
    context.codeStream[context.ip] = pkgIdx;
    if (fn.packageName) this.identTable.add(this.globalStringTable, fn.packageName.literal, context.ip);
    context.ip++;
    this.emit(context, fn.stmts.length > 0 ? 1 : 0);
    const endJmpIp = context.ip++;
    this.emit(context, fn.args.length);
    for (const arg of fn.args) {
      const strIdx = this.globalStringTable.add((arg.vtype === 0 /* Global */ ? "$" : "%") + arg.name.literal, false, false);
      context.codeStream[context.ip] = strIdx;
      this.identTable.add(this.globalStringTable, (arg.vtype === 0 /* Global */ ? "$" : "%") + arg.name.literal, context.ip);
      context.ip++;
    }
    const savedBreak = context.breakPoint, savedCont = context.continuePoint;
    context.breakPoint = 0;
    context.continuePoint = 0;
    this.compileBlock(context, fn.stmts);
    context.breakPoint = savedBreak;
    context.continuePoint = savedCont;
    this.emit(context, this.getOpcodeValue(13 /* Return */));
    context.codeStream[endJmpIp] = context.ip;
    this.inFunction = false;
    this.currentStringTable = prevS;
    this.currentFloatTable = prevF;
  }
  compilePackage(context, pkg) {
    for (const fn of pkg.functions) {
      fn.packageName = pkg.name;
      this.compileFunction(context, fn);
    }
  }
  // --- Expression compilation ---
  compileExpr(context, expr, typeReq) {
    if (expr instanceof IntExpr) this.compileIntExpr(context, expr, typeReq);
    else if (expr instanceof FloatExpr) this.compileFloatExpr(context, expr, typeReq);
    else if (expr instanceof StringConstExpr) this.compileStringExpr(context, expr, typeReq);
    else if (expr instanceof ConstantExpr) this.compileConstantExpr(context, expr, typeReq);
    else if (expr instanceof FloatBinaryExpr) this.compileFloatBinary(context, expr, typeReq);
    else if (expr instanceof IntBinaryExpr) this.compileIntBinary(context, expr, typeReq);
    else if (expr instanceof StrCatExpr) this.compileStrCat(context, expr, typeReq);
    else if (expr instanceof StrEqExpr) this.compileStrEq(context, expr, typeReq);
    else if (expr instanceof VarExpr) this.compileVarExpr(context, expr, typeReq);
    else if (expr instanceof AssignExpr) this.compileAssign(context, expr, typeReq);
    else if (expr instanceof AssignOpExpr) this.compileAssignOp(context, expr, typeReq);
    else if (expr instanceof FuncCallExpr) this.compileFuncCall(context, expr, typeReq);
    else if (expr instanceof SlotAccessExpr) this.compileSlotAccess(context, expr, typeReq);
    else if (expr instanceof SlotAssignExpr) this.compileSlotAssign(context, expr, typeReq);
    else if (expr instanceof ObjectDeclExpr) this.compileObjectDecl(context, expr, typeReq);
    else if (expr instanceof ParenthesisExpr) this.compileExpr(context, expr.expr, typeReq);
    else if (expr instanceof IntUnaryExpr) this.compileIntUnary(context, expr, typeReq);
    else if (expr instanceof FloatUnaryExpr) this.compileFloatUnary(context, expr, typeReq);
    else if (expr instanceof ConditionalExpr) this.compileConditional(context, expr, typeReq);
  }
  compileIntExpr(c, e, t) {
    if (t === 0 /* None */) return;
    if (t === 3 /* String */) {
      const i = this.currentStringTable.add(String(e.value), true, false);
      this.emit(c, this.getOpcodeValue(68 /* LoadImmedStr */), i);
    } else if (t === 2 /* Float */) {
      const i = this.addFloat(e.value);
      this.emit(c, this.getOpcodeValue(66 /* LoadImmedFlt */), i);
    } else this.emit(c, this.getOpcodeValue(65 /* LoadImmedUInt */), e.value);
  }
  compileFloatExpr(c, e, t) {
    if (t === 0 /* None */) return;
    if (t === 3 /* String */) {
      const i = this.currentStringTable.add(String(e.value), true, false);
      this.emit(c, this.getOpcodeValue(68 /* LoadImmedStr */), i);
    } else if (t === 2 /* Float */) {
      const i = this.addFloat(e.value);
      this.emit(c, this.getOpcodeValue(66 /* LoadImmedFlt */), i);
    } else this.emit(c, this.getOpcodeValue(65 /* LoadImmedUInt */), e.value | 0);
  }
  compileStringExpr(c, e, t) {
    if (t === 0 /* None */) return;
    if (t === 3 /* String */) {
      const i = this.currentStringTable.add(e.value, true, e.tag);
      this.emit(c, e.tag ? this.getOpcodeValue(67 /* TagToStr */) : this.getOpcodeValue(68 /* LoadImmedStr */), i);
    } else {
      const v = this.stringToNumber(e.value);
      if (t === 2 /* Float */) {
        const i = this.addFloat(v);
        this.emit(c, this.getOpcodeValue(66 /* LoadImmedFlt */), i);
      } else this.emit(c, this.getOpcodeValue(65 /* LoadImmedUInt */), v | 0);
    }
  }
  compileConstantExpr(c, e, t) {
    if (t === 0 /* None */) return;
    if (t === 3 /* String */) {
      const i = this.currentStringTable.add(e.name.literal, false, false);
      this.emit(c, this.getOpcodeValue(68 /* LoadImmedStr */), i);
    } else {
      const v = this.stringToNumber(e.name.literal);
      if (t === 2 /* Float */) {
        const i = this.addFloat(v);
        this.emit(c, this.getOpcodeValue(66 /* LoadImmedFlt */), i);
      } else this.emit(c, this.getOpcodeValue(65 /* LoadImmedUInt */), v | 0);
    }
  }
  compileFloatBinary(c, e, t) {
    this.compileExpr(c, e.right, 2 /* Float */);
    this.compileExpr(c, e.left, 2 /* Float */);
    this.emit(c, this.getOpcodeValue(this.getFloatBinOp(e.op.type)));
    if (t !== 2 /* Float */) this.emit(c, this.getOpcodeValue(this.conversionOp(2 /* Float */, t)));
  }
  compileIntBinary(c, e, t) {
    this.compileExpr(c, e.right, 1 /* Int */);
    this.compileExpr(c, e.left, 1 /* Int */);
    this.emit(c, this.getOpcodeValue(this.getIntBinOp(e.op.type)));
    if (t !== 1 /* Int */) this.emit(c, this.getOpcodeValue(this.conversionOp(1 /* Int */, t)));
  }
  compileStrCat(c, e, t) {
    this.compileExpr(c, e.left, 3 /* String */);
    if (e.op.type === 41 /* Concat */) this.emit(c, this.getOpcodeValue(73 /* AdvanceStr */));
    else {
      this.emit(c, this.getOpcodeValue(74 /* AdvanceStrAppendChar */));
      this.emit(c, e.op.type === 42 /* SpaceConcat */ ? 32 : e.op.type === 43 /* TabConcat */ ? 9 : 10);
    }
    this.compileExpr(c, e.right, 3 /* String */);
    this.emit(c, this.getOpcodeValue(77 /* RewindStr */));
    if (t !== 3 /* String */) this.emit(c, this.getOpcodeValue(this.conversionOp(3 /* String */, t)));
  }
  compileStrEq(c, e, t) {
    this.compileExpr(c, e.left, 3 /* String */);
    this.emit(c, this.getOpcodeValue(76 /* AdvanceStrNul */));
    this.compileExpr(c, e.right, 3 /* String */);
    this.emit(c, this.getOpcodeValue(79 /* CompareStr */));
    if (e.op.type === 40 /* StringNotEquals */) this.emit(c, this.getOpcodeValue(24 /* Not */));
    if (t !== 1 /* Int */) this.emit(c, this.getOpcodeValue(this.conversionOp(1 /* Int */, t)));
  }
  compileVarExpr(c, e, t) {
    if (t === 0 /* None */) return;
    const prefix = e.vtype === 0 /* Global */ ? "$" : "%";
    const ident = e.namespace ? prefix + e.namespace.literal + "::" + e.name.literal : prefix + e.name.literal;
    if (e.arrayIndex) {
      this.emit(c, this.getOpcodeValue(69 /* LoadImmedIdent */));
      const strIdx = this.currentStringTable.add(ident, false, false);
      c.codeStream[c.ip] = strIdx;
      this.identTable.add(this.currentStringTable, ident, c.ip);
      c.ip++;
      this.emit(c, this.getOpcodeValue(73 /* AdvanceStr */));
      this.compileExpr(c, e.arrayIndex, 3 /* String */);
      this.emit(c, this.getOpcodeValue(77 /* RewindStr */));
      this.emit(c, this.getOpcodeValue(38 /* SetCurVarArray */));
    } else {
      this.emit(c, this.getOpcodeValue(36 /* SetCurVar */));
      const strIdx = this.currentStringTable.add(ident, false, false);
      c.codeStream[c.ip] = strIdx;
      this.identTable.add(this.currentStringTable, ident, c.ip);
      c.ip++;
    }
    if (t === 1 /* Int */) this.emit(c, this.getOpcodeValue(40 /* LoadVarUInt */));
    else if (t === 2 /* Float */) this.emit(c, this.getOpcodeValue(41 /* LoadVarFlt */));
    else this.emit(c, this.getOpcodeValue(42 /* LoadVarStr */));
  }
  compileAssign(c, e, t) {
    const sub = e.expr instanceof IntExpr ? 1 /* Int */ : e.expr instanceof FloatExpr ? 2 /* Float */ : 3 /* String */;
    this.compileExpr(c, e.expr, sub);
    const ident = e.varExpr.namespace ? (e.varExpr.vtype === 0 /* Global */ ? "$" : "%") + e.varExpr.namespace.literal + "::" + e.varExpr.name.literal : (e.varExpr.vtype === 0 /* Global */ ? "$" : "%") + e.varExpr.name.literal;
    if (e.varExpr.arrayIndex) {
      if (sub === 3 /* String */) this.emit(c, this.getOpcodeValue(73 /* AdvanceStr */));
      this.emit(c, this.getOpcodeValue(69 /* LoadImmedIdent */));
      const idIp = this.context_ip(c);
      this.identTable.add(this.currentStringTable, ident, idIp);
      this.emit(c, this.getOpcodeValue(73 /* AdvanceStr */));
      this.compileExpr(c, e.varExpr.arrayIndex, 3 /* String */);
      this.emit(c, this.getOpcodeValue(77 /* RewindStr */));
      this.emit(c, this.getOpcodeValue(39 /* SetCurVarArrayCreate */));
      if (sub === 3 /* String */) this.emit(c, this.getOpcodeValue(78 /* TerminateRewindStr */));
    } else {
      this.emit(c, this.getOpcodeValue(37 /* SetCurVarCreate */));
      const idIp = this.context_ip(c);
      this.identTable.add(this.currentStringTable, ident, idIp);
    }
    if (sub === 3 /* String */) this.emit(c, this.getOpcodeValue(45 /* SaveVarStr */));
    else if (sub === 1 /* Int */) this.emit(c, this.getOpcodeValue(43 /* SaveVarUInt */));
    else this.emit(c, this.getOpcodeValue(44 /* SaveVarFlt */));
    if (t !== sub) this.emit(c, this.getOpcodeValue(this.conversionOp(sub, t)));
  }
  compileAssignOp(c, e, t) {
    const { subType, operand } = this.getAssignOpInfo(e.op.type);
    if (e.expr) {
      this.compileExpr(c, e.expr, subType);
    } else {
      this.compileIntExpr(c, new IntExpr(e.lineNo, 1), subType);
    }
    const ident = e.varExpr.namespace ? (e.varExpr.vtype === 0 /* Global */ ? "$" : "%") + e.varExpr.namespace.literal + "::" + e.varExpr.name.literal : (e.varExpr.vtype === 0 /* Global */ ? "$" : "%") + e.varExpr.name.literal;
    if (e.varExpr.arrayIndex) {
      this.emit(c, this.getOpcodeValue(69 /* LoadImmedIdent */));
      const idIp = this.context_ip(c);
      this.identTable.add(this.currentStringTable, ident, idIp);
      this.emit(c, this.getOpcodeValue(73 /* AdvanceStr */));
      this.compileExpr(c, e.varExpr.arrayIndex, 3 /* String */);
      this.emit(c, this.getOpcodeValue(77 /* RewindStr */));
      this.emit(c, this.getOpcodeValue(39 /* SetCurVarArrayCreate */));
    } else {
      this.emit(c, this.getOpcodeValue(37 /* SetCurVarCreate */));
      const idIp = this.context_ip(c);
      this.identTable.add(this.currentStringTable, ident, idIp);
    }
    this.emit(c, this.getOpcodeValue(subType === 2 /* Float */ ? 41 /* LoadVarFlt */ : 40 /* LoadVarUInt */));
    this.emit(c, this.getOpcodeValue(operand));
    this.emit(c, this.getOpcodeValue(subType === 2 /* Float */ ? 44 /* SaveVarFlt */ : 43 /* SaveVarUInt */));
    if (t !== subType) this.emit(c, this.getOpcodeValue(this.conversionOp(subType, t)));
  }
  compileFuncCall(c, e, t) {
    this.emit(c, this.getOpcodeValue(81 /* PushFrame */));
    if (e.callType === 1 /* MethodCall */ && e.objectExpr) {
      this.compileExpr(c, e.objectExpr, 3 /* String */);
      this.emit(c, this.getOpcodeValue(80 /* Push */));
    }
    for (const arg of e.args) {
      this.compileExpr(c, arg, 3 /* String */);
      this.emit(c, this.getOpcodeValue(80 /* Push */));
    }
    this.emit(c, e.callType === 1 /* MethodCall */ || e.callType === 2 /* ParentCall */ ? this.getOpcodeValue(71 /* CallFunc */) : this.getOpcodeValue(70 /* CallFuncResolve */));
    const nameIp = this.context_ip(c);
    this.identTable.add(this.currentStringTable, e.name.literal, nameIp);
    const nsIp = this.context_ip(c);
    if (e.namespace) this.identTable.add(this.currentStringTable, e.namespace.literal, nsIp);
    this.emit(c, e.callType);
    if (t !== 3 /* String */) this.emit(c, this.getOpcodeValue(this.conversionOp(3 /* String */, t)));
  }
  compileSlotAccess(c, e, t) {
    if (t === 0 /* None */) return;
    if (e.arrayExpr) {
      this.compileExpr(c, e.arrayExpr, 3 /* String */);
      this.emit(c, this.getOpcodeValue(73 /* AdvanceStr */));
    }
    this.compileExpr(c, e.objectExpr, 3 /* String */);
    this.emit(c, this.getOpcodeValue(46 /* SetCurObject */));
    this.emit(c, this.getOpcodeValue(48 /* SetCurField */));
    const fIp = this.context_ip(c);
    this.identTable.add(this.currentStringTable, e.slotName.literal, fIp);
    if (e.arrayExpr) {
      this.emit(c, this.getOpcodeValue(78 /* TerminateRewindStr */));
      this.emit(c, this.getOpcodeValue(49 /* SetCurFieldArray */));
    }
    if (t === 1 /* Int */) this.emit(c, this.getOpcodeValue(50 /* LoadFieldUInt */));
    else if (t === 2 /* Float */) this.emit(c, this.getOpcodeValue(51 /* LoadFieldFlt */));
    else this.emit(c, this.getOpcodeValue(52 /* LoadFieldStr */));
  }
  compileSlotAssign(c, e, t) {
    this.compileExpr(c, e.expr, 3 /* String */);
    this.emit(c, this.getOpcodeValue(73 /* AdvanceStr */));
    if (e.arrayExpr) {
      this.compileExpr(c, e.arrayExpr, 3 /* String */);
      this.emit(c, this.getOpcodeValue(73 /* AdvanceStr */));
    }
    if (e.objectExpr) {
      this.compileExpr(c, e.objectExpr, 3 /* String */);
      this.emit(c, this.getOpcodeValue(46 /* SetCurObject */));
    } else this.emit(c, this.getOpcodeValue(47 /* SetCurObjectNew */));
    this.emit(c, this.getOpcodeValue(48 /* SetCurField */));
    const fIp = this.context_ip(c);
    this.identTable.add(this.currentStringTable, e.slotName.literal, fIp);
    if (e.arrayExpr) {
      this.emit(c, this.getOpcodeValue(78 /* TerminateRewindStr */));
      this.emit(c, this.getOpcodeValue(49 /* SetCurFieldArray */));
    }
    this.emit(c, this.getOpcodeValue(78 /* TerminateRewindStr */));
    this.emit(c, this.getOpcodeValue(55 /* SaveFieldStr */));
    if (t !== 3 /* String */) this.emit(c, this.getOpcodeValue(this.conversionOp(3 /* String */, t)));
  }
  compileObjectDecl(c, e, t) {
    this.compileSubObject(c, e, true);
    if (t !== 1 /* Int */) this.emit(c, this.getOpcodeValue(this.conversionOp(1 /* Int */, t)));
  }
  compileSubObject(c, e, root) {
    const start = c.ip;
    this.emit(c, this.getOpcodeValue(81 /* PushFrame */));
    const classNameExpr = e.className;
    const classStrIdx = this.currentStringTable.add(classNameExpr.name.literal, false, false);
    this.emit(c, this.getOpcodeValue(69 /* LoadImmedIdent */));
    const classIp = this.context_ip(c);
    this.identTable.add(this.currentStringTable, classNameExpr.name.literal, classIp);
    this.emit(c, this.getOpcodeValue(80 /* Push */));
    if (e.objectNameExpr instanceof ConstantExpr) {
      this.emit(c, this.getOpcodeValue(69 /* LoadImmedIdent */));
      const objIp = this.context_ip(c);
      const objNameExpr = e.objectNameExpr;
      this.identTable.add(this.currentStringTable, objNameExpr.name.literal, objIp);
    } else {
      this.compileExpr(c, e.objectNameExpr, 3 /* String */);
    }
    this.emit(c, this.getOpcodeValue(80 /* Push */));
    for (const arg of e.args) {
      this.compileExpr(c, arg, 3 /* String */);
      this.emit(c, this.getOpcodeValue(80 /* Push */));
    }
    this.emit(c, this.getOpcodeValue(1 /* CreateObject */));
    const pIp = this.context_ip(c);
    if (e.parentObject) this.identTable.add(this.currentStringTable, e.parentObject.literal, pIp);
    this.emit(c, e.structDecl ? 1 : 0);
    const failIp = this.context_ip(c);
    for (const slot of e.slotDecls) this.compileExpr(c, slot, 0 /* None */);
    this.emit(c, this.getOpcodeValue(4 /* AddObject */));
    this.emit(c, root ? 1 : 0);
    for (const sub of e.subObjects) this.compileSubObject(c, sub, false);
    this.emit(c, this.getOpcodeValue(5 /* EndObject */));
    this.emit(c, root || e.structDecl ? 1 : 0);
    c.codeStream[failIp] = c.ip;
  }
  compileIntUnary(c, e, t) {
    this.compileExpr(c, e.expr, 1 /* Int */);
    if (e.op.type === 35 /* Not */) this.emit(c, this.getOpcodeValue(24 /* Not */));
    else if (e.op.type === 38 /* Tilde */) this.emit(c, this.getOpcodeValue(26 /* OnesComplement */));
    if (t !== 1 /* Int */) this.emit(c, this.getOpcodeValue(this.conversionOp(1 /* Int */, t)));
  }
  compileFloatUnary(c, e, t) {
    this.compileExpr(c, e.expr, 2 /* Float */);
    this.emit(c, this.getOpcodeValue(35 /* Neg */));
    if (t !== 2 /* Float */) this.emit(c, this.getOpcodeValue(this.conversionOp(2 /* Float */, t)));
  }
  compileConditional(c, e, t) {
    this.compileExpr(c, e.condition, 1 /* Int */);
    this.emit(c, this.getOpcodeValue(7 /* JmpIfNot */));
    const elseJmpIp = c.ip++;
    this.compileExpr(c, e.trueExpr, t);
    this.emit(c, this.getOpcodeValue(12 /* Jmp */));
    const endJmpIp = c.ip++;
    c.codeStream[elseJmpIp] = c.ip;
    this.compileExpr(c, e.falseExpr, t);
    c.codeStream[endJmpIp] = c.ip;
  }
  // --- Helpers ---
  emit(c, ...ops) {
    const invalid = this.ops.invalid;
    for (const op of ops) {
      c.codeStream[c.ip++] = op === invalid ? this.getOpcodeValue(13 /* Return */) : op;
    }
  }
  context_ip(c) {
    const ip = c.ip;
    c.ip++;
    return ip;
  }
  addBreakLine(c, lineNo) {
    if (this.inFunction) {
      const line = this.breakLineCount * 2;
      this.breakLineCount++;
      if (c.lineBreakPairs.length > 0) {
        c.lineBreakPairs[line] = lineNo;
        c.lineBreakPairs[line + 1] = c.ip;
      }
    }
  }
  stringToNumber(value) {
    if (value === "true") return 1;
    if (value === "false") return 0;
    const v = parseFloat(value);
    return isNaN(v) ? 0 : v;
  }
  addFloat(value) {
    const idx = this.currentFloatTable.indexOf(value);
    if (idx >= 0) return idx;
    this.currentFloatTable.push(value);
    return this.currentFloatTable.length - 1;
  }
  getFloatBinOp(tt) {
    switch (tt) {
      case 15 /* Plus */:
        return 31 /* Add */;
      case 16 /* Minus */:
        return 32 /* Sub */;
      case 17 /* Multiply */:
        return 33 /* Mul */;
      case 18 /* Divide */:
        return 34 /* Div */;
      default:
        return 83 /* Invalid */;
    }
  }
  getIntBinOp(tt) {
    switch (tt) {
      case 52 /* BitwiseXor */:
        return 20 /* Xor */;
      case 19 /* Modulus */:
        return 21 /* Mod */;
      case 50 /* BitwiseAnd */:
        return 22 /* BitAnd */;
      case 51 /* BitwiseOr */:
        return 23 /* BitOr */;
      case 31 /* LessThan */:
        return 17 /* CmpLT */;
      case 33 /* LessThanEqual */:
        return 18 /* CmpLE */;
      case 32 /* GreaterThan */:
        return 15 /* CmpGT */;
      case 34 /* GreaterThanEqual */:
        return 16 /* CmpGE */;
      case 37 /* Equal */:
        return 14 /* CmpEQ */;
      case 36 /* NotEqual */:
        return 19 /* CmpNE */;
      case 47 /* LogicalOr */:
        return 30 /* Or */;
      case 46 /* LogicalAnd */:
        return 29 /* And */;
      case 49 /* RightBitShift */:
        return 27 /* Shr */;
      case 48 /* LeftBitShift */:
        return 28 /* Shl */;
      default:
        return 83 /* Invalid */;
    }
  }
  getAssignOpInfo(tt) {
    switch (tt) {
      case 21 /* PlusAssign */:
        return { subType: 2 /* Float */, operand: 31 /* Add */ };
      case 22 /* MinusAssign */:
        return { subType: 2 /* Float */, operand: 32 /* Sub */ };
      case 23 /* MultiplyAssign */:
        return { subType: 2 /* Float */, operand: 33 /* Mul */ };
      case 28 /* DivideAssign */:
        return { subType: 2 /* Float */, operand: 34 /* Div */ };
      case 27 /* ModulusAssign */:
        return { subType: 1 /* Int */, operand: 21 /* Mod */ };
      case 25 /* AndAssign */:
        return { subType: 1 /* Int */, operand: 22 /* BitAnd */ };
      case 26 /* XorAssign */:
        return { subType: 1 /* Int */, operand: 20 /* Xor */ };
      case 24 /* OrAssign */:
        return { subType: 1 /* Int */, operand: 23 /* BitOr */ };
      case 29 /* ShiftLeftAssign */:
        return { subType: 1 /* Int */, operand: 28 /* Shl */ };
      case 30 /* ShiftRightAssign */:
        return { subType: 1 /* Int */, operand: 27 /* Shr */ };
      case 73 /* PlusPlus */:
        return { subType: 2 /* Float */, operand: 31 /* Add */ };
      case 74 /* MinusMinus */:
        return { subType: 2 /* Float */, operand: 32 /* Sub */ };
      default:
        return { subType: 1 /* Int */, operand: 83 /* Invalid */ };
    }
  }
  conversionOp(src, dest) {
    if (src === 3 /* String */ && dest === 1 /* Int */) return 56 /* StrToUInt */;
    if (src === 3 /* String */ && dest === 2 /* Float */) return 57 /* StrToFlt */;
    if (src === 3 /* String */ && dest === 0 /* None */) return 58 /* StrToNone */;
    if (src === 2 /* Float */ && dest === 1 /* Int */) return 59 /* FltToUInt */;
    if (src === 2 /* Float */ && dest === 3 /* String */) return 60 /* FltToStr */;
    if (src === 2 /* Float */ && dest === 0 /* None */) return 61 /* FltToNone */;
    if (src === 1 /* Int */ && dest === 2 /* Float */) return 62 /* UIntToFlt */;
    if (src === 1 /* Int */ && dest === 3 /* String */) return 63 /* UIntToStr */;
    if (src === 1 /* Int */ && dest === 0 /* None */) return 64 /* UIntToNone */;
    return 83 /* Invalid */;
  }
  // --- Binary serialization (DSO format) ---
  serialize(context, codeSize, lineBreakPairCount) {
    const buf = new ArrayBuffer(1024 * 1024);
    const view = new DataView(buf);
    let pos = 0;
    const writeU32 = (v) => {
      view.setUint32(pos, v, true);
      pos += 4;
    };
    const writeF64 = (v) => {
      view.setFloat64(pos, v, true);
      pos += 8;
    };
    const writeBytes = (bytes) => {
      for (const b of bytes) {
        view.setUint8(pos++, b);
      }
    };
    writeU32(this.dsoVersion);
    writeU32(this.globalStringTable.totalLen);
    for (const entry of this.globalStringTable.entries) {
      for (let i = 0; i < entry.string.length; i++) view.setUint8(pos++, entry.string.charCodeAt(i));
      view.setUint8(pos++, 0);
      for (let i = entry.string.length + 1; i < entry.len; i++) view.setUint8(pos++, 0);
    }
    writeU32(this.globalFloatTable.length);
    for (const f of this.globalFloatTable) writeF64(f);
    writeU32(this.functionStringTable.totalLen);
    for (const entry of this.functionStringTable.entries) {
      for (let i = 0; i < entry.string.length; i++) view.setUint8(pos++, entry.string.charCodeAt(i));
      view.setUint8(pos++, 0);
      for (let i = entry.string.length + 1; i < entry.len; i++) view.setUint8(pos++, 0);
    }
    writeU32(this.functionFloatTable.length);
    for (const f of this.functionFloatTable) writeF64(f);
    writeU32(codeSize);
    writeU32(lineBreakPairCount / 2);
    for (let i = 0; i < codeSize; i++) {
      const v = context.codeStream[i];
      if (v <= 255) view.setUint8(pos++, v);
      else {
        view.setUint8(pos++, 255);
        view.setUint32(pos, v, true);
        pos += 4;
      }
    }
    for (let i = 0; i < lineBreakPairCount; i++) writeU32(context.lineBreakPairs[i]);
    writeU32(this.identTable.identMap.size);
    for (const [strIdx, positions] of this.identTable.identMap) {
      writeU32(strIdx);
      writeU32(positions.length);
      for (const p of positions) writeU32(p);
    }
    return new Uint8Array(buf, 0, pos);
  }
};

// cli.ts
var args = process.argv.slice(2);
if (args.length < 1) {
  console.error("Usage: node cli.js <source.cs> [output.dso]");
  process.exit(1);
}
var fs = require("fs");
var sourcePath = args[0];
var outPath = args[1] || sourcePath + ".dso";
var code = fs.readFileSync(sourcePath, "utf-8");
var compiler = new Compiler("TGE10");
var dso = compiler.compile(code);
fs.writeFileSync(outPath, Buffer.from(dso));
console.log(`Compiled ${sourcePath} -> ${outPath} (${dso.length} bytes)`);
