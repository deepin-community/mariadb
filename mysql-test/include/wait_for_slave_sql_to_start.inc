# ==== Purpose ====
#
# Waits the SQL thread of the current connection has started, or until
# a timeout is reached.
#
# ==== Usage ====
#
# [--let $slave_timeout= NUMBER]
# [--let $rpl_debug= 1]
# --source include/wait_for_slave_sql_to_start.inc
#
# Parameters:
#   $slave_timeout
#     Timeout used when waiting for the slave SQL thread to start.
#     See include/wait_for_slave_param.inc
#
#   $rpl_debug
#     See include/rpl_init.inc


--let $include_filename= wait_for_slave_sql_to_start.inc
--source include/begin_include_file.inc


let $slave_param= Slave_SQL_Running;
let $slave_param_value= Yes;

# Unfortunately, the slave sql thread sets Slave_SQL_Running=Yes
# *before* it clears Last_SQL_Errno. So we have to allow errors in
# the SQL thread here.

#--let $slave_error_param= Last_SQL_Errno
source include/wait_for_slave_param.inc;
#--let $slave_error_param=


--let $include_filename= wait_for_slave_sql_to_start.inc
--source include/end_include_file.inc
