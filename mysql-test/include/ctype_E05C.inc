--echo # Start of ctype_E05C.inc

#
# A shared test for character sets big5, cp932, gbk, sjis
# They all can have 0x5C as the second byte in a multi-byte character.
# 0xE05C is one of such characters.
#

# Checking that the character 0xE05C correctly understands 5C as the second byte
# rather than a stand-alone backslash, including the strings that also
# have real backslash escapes and/or separator escapes.

SELECT HEX('�\'),HEX('�\t');
SELECT HEX('\\�\'),HEX('\\�\t'),HEX('\\�\t\t');
SELECT HEX('''�\'),HEX('�\''');
SELECT HEX('\\''�\'),HEX('�\''\\');

SELECT HEX(BINARY('�\')),HEX(BINARY('�\t'));
SELECT HEX(BINARY('\\�\')),HEX(BINARY('\\�\t')),HEX(BINARY('\\�\t\t'));
SELECT HEX(BINARY('''�\')),HEX(BINARY('�\'''));
SELECT HEX(BINARY('\\''�\')),HEX(BINARY('�\''\\'));

SELECT HEX(_BINARY'�\'),HEX(_BINARY'�\t');
SELECT HEX(_BINARY'\\�\'),HEX(_BINARY'\\�\t'),HEX(_BINARY'\\�\t\t');
SELECT HEX(_BINARY'''�\'),HEX(_BINARY'�\''');
SELECT HEX(_BINARY'\\''�\'),HEX(_BINARY'�\''\\');

CREATE TABLE t1 AS SELECT REPEAT(' ',10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('�\'),('�\t');
INSERT INTO t1 VALUES ('\\�\'),('\\�\t'),('\\�\t\t');
INSERT INTO t1 VALUES ('''�\'),('�\''');
INSERT INTO t1 VALUES ('\\''�\'),('�\''\\');
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 (a BLOB);
INSERT INTO t1 VALUES ('�\'),('�\t');
INSERT INTO t1 VALUES ('\\�\'),('\\�\t'),('\\�\t\t');
INSERT INTO t1 VALUES ('''�\'),('�\''');
INSERT INTO t1 VALUES ('\\''�\'),('�\''\\');
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT REPEAT(' ', 10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES (BINARY('�\')),(BINARY('�\t'));
INSERT INTO t1 VALUES (BINARY('\\�\')),(BINARY('\\�\t')),(BINARY('\\�\t\t'));
INSERT INTO t1 VALUES (BINARY('''�\')),(BINARY('�\'''));
INSERT INTO t1 VALUES (BINARY('\\''�\')),(BINARY('�\''\\'));
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 (a BLOB);
INSERT INTO t1 VALUES (BINARY('�\')),(BINARY('�\t'));
INSERT INTO t1 VALUES (BINARY('\\�\')),(BINARY('\\�\t')),(BINARY('\\�\t\t'));
INSERT INTO t1 VALUES (BINARY('''�\')),(BINARY('�\'''));
INSERT INTO t1 VALUES (BINARY('\\''�\')),(BINARY('�\''\\'));
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT REPEAT(' ', 10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES (_BINARY'�\'),(_BINARY'�\t');
INSERT INTO t1 VALUES (_BINARY'\\�\'),(_BINARY'\\�\t'),(_BINARY'\\�\t\t');
INSERT INTO t1 VALUES (_BINARY'''�\'),(_BINARY'�\''');
INSERT INTO t1 VALUES (_BINARY'\\''�\'),(_BINARY'�\''\\');
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 (a BLOB);
INSERT INTO t1 VALUES (_BINARY'�\'),(_BINARY'�\t');
INSERT INTO t1 VALUES (_BINARY'\\�\'),(_BINARY'\\�\t'),(_BINARY'\\�\t\t');
INSERT INTO t1 VALUES (_BINARY'''�\'),(_BINARY'�\''');
INSERT INTO t1 VALUES (_BINARY'\\''�\'),(_BINARY'�\''\\');
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

#
# test how strings are written into view's frm
#
disable_view_protocol;
create view v1 as select hex('�\'), hex('�\t');
select * from v1;
drop view v1;
enable_view_protocol;

# Checking that with character_set_client=binary 0x5C in 0xE05C
# is treated as escape rather than the second byte of a multi-byte character,
# even if character_set_connection is big5/cp932/gbk/sjis.
# Note, the other 0x5C which is before 0xE05C is also treated as escape.
#
#check after fix MDEV-29290
--disable_view_protocol
SET character_set_client=binary, character_set_results=binary;
SELECT @@character_set_client, @@character_set_connection, @@character_set_results;
SELECT HEX('�\['), HEX('\�\[');
CREATE TABLE t1 AS SELECT REPEAT(' ', 10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('�\['),('\�\[');
SELECT HEX(a) FROM t1;
DROP TABLE t1;

#
# Checking the other way around:
# 0x5C in 0xE05C is treated as the second byte
# when character_set_client=big5,cp932,gbk,sjis
# and character_set_connection=binary
#
SET character_set_client=@@character_set_connection, character_set_results=@@character_set_connection;
SET character_set_connection=binary;
SELECT @@character_set_client, @@character_set_connection, @@character_set_results;
SELECT HEX('�\['), HEX('\�\[');
CREATE TABLE t1 AS SELECT REPEAT(' ', 10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('�\['),('\�\[');
SELECT HEX(a) FROM t1;
DROP TABLE t1;
--enable_view_protocol

--echo # End of ctype_E05C.inc

