
$sendmail = "/usr/sbin/sendmail -t -odb -oem";
$day = "17-21:00, 5-8";
$period = 300;
$cycle = 20; # ms
$mysql_host = "localhost";
$mysql_user = "zelio";
$mysql_passwd = "********";
$db = "zelio";
#$rbase = 24;	# for serial
#$wbase = 0;	# for serial
$rbase = 20;	# for eth
$wbase = 16;	# for eth
$wvar = "/usr/local/var/modbus.db";
# on summer: radiators off, only warm floor
# on summer: set boiler only by time, ignore temp
$summer = 0;

use Time::HiRes qw(gettimeofday usleep);
use DBI;
use POSIX;
use DB_File;

sub init
{
	%rvar = (
		"t1" => "",
		"t2" => "",
		"termostat" => 0,
		"electro_cnt" => "");
	tie(%wvar, 'DB_File', $wvar, O_RDWR) ||
		tie(%wvar, 'DB_File', $wvar, O_RDWR|O_CREAT, 0644) ||
			logwrite(1, "Cannot tie: $!");
	$wvar{"mint1"} = "18.0" unless $wvar{"mint1"};
	$wvar{"mint2"} = "20.0" unless $wvar{"mint2"};
	$wvar{"boiler"} = "0" unless defined($wvar{"boiler"});

	connect_mysql();
	($prev_periodic, $last_el_cnt) = select_mysql("select unixtime, el_cnt from data order by time desc limit 1");
	disconnect_mysql();
	logwrite(1, "Perl init() done");
}

sub deinit
{
	untie(%wvar);
}

sub hello
{
	return "R Hello\n";
}

sub command
{
# return response line
# set $bye=1 if it was last command
	my ($command) = @_;
	my ($cmd, $res, $i, $var);

	#logwrite(3, "command: $command");
	if ($command =~ /^(get|getavg)(?:\s+([a-z0-9_]+))?\s*$/i) {
		($cmd, $var) = ($1, $2);
		if ($var eq "" || $var =~ /^all$/i) {
			$res = "";
			$i=0;
			foreach (sort keys %rvar) {
				$res .= (++$i < keys %rvar || keys %wvar) ? "r" : "R";
				if ($cmd eq "getavg" && defined($avg{$_})) {
					$res .= " $_: $avg{$_}\n";
				} else {
					$res .= " $_: $rvar{$_}\n";
				}
			}
			$i=0;
			foreach (sort keys %wvar) {
				$res .= (++$i < keys %wvar) ? "r" : "R";
				if (defined($tempwvar{$_})) {
					$res .= " $_: $wvar{$_} ";
					$res .= "(temporary set $tempwvar{$_} until " . localtime($tempwtime{$_}) . ")\n";
				} else {
					$res .= " $_: $wvar{$_}\n";
				}
			}
		} elsif ($cmd eq "getavg" && defined($avg{$var})) {
			$res = "R $var: $avg{$var}\n";
		} elsif ($rvar{$var}) {
			$res = "R $var: $rvar{$var}\n";
		} elsif ($wvar{$var}) {
			$res = "R $var: $wvar{$var}";
			if (defined($tempwvar{$var})) {
				$res .= " (temporary set $tempwvar{$var} until " . localtime($tempwtime{$var}) . ")";
			}
			$res .= "\n";
		} else {
			$res = "E Unknown variable $var\n";
		}
	} elsif ($command =~ /^set\s+([a-z0-9_]+)=(\S+)\s*$/i) {
		($var, $val) = ($1, $2);
		if (!defined($wvar{$var})) {
			$res = "E Unknown variable $var\n";
		} else {
			$wvar{$var} = $val;
			$res = "R ok\n";
		}
		# write registers
		# ...
	} elsif ($command =~ /^tempset\s+(\d+(?::\d+)?)\s+([a-z0-9_]+)=(\S+)\s*$/i) {
		($time, $var, $val) = ($1, $2, $3);
		if (!defined($wvar{$var})) {
			$res = "E Unknown variable $var\n";
		} else {
			if ($time =~ /:/) {
				$duration = $`*60+$';
			} else {
				$duration = $time*60;
			}
			$tempwvar{$var} = $val;
			$tempwtime{$var} = time()+$duration*60;
			$res = "R ok\n";
		}
		# write registers
		# ...
	} elsif ($command =~ /^(q|quit|bye)\s*$/i) {
		$res = "R bye\n";
		$bye = 1;
	} elsif ($command =~ /^debug\s+(\d+)$/i) {
		$debug = $1;
		$res = "R ok\n";
	} elsif ($command =~ /^debug\s*$/i) {
		$res = "R debug level is $debug\n";
	} elsif ($command =~ /^help\s*$/i) {
		$res = "r Possible commands:\n";
		$res .= "r get               - show all variables,\n";
		$res .= "r get all           - show all variables,\n";
		$res .= "r get <var>         - show one variable,\n";
		$res .= "r getavg <var>      - show average variable value,\n";
		$res .= "r set <var>=<value> - set variable value,\n";
		$res .= "r tempset hh[:mm] <var>=<value> - temporary set variable value,\n";
		$res .= "r debug <level>     - set debug level.\n";
		$res .= "r (first 'r' char in the lines is not part of this help)\n";
		$res .= "r example:   set mint1=18.5\n";
		$res .= "r example:   get boiler\n";
		$res .= "R example:   get\n";
	} else {
		$res = "E What?\n";
		$debug = 0;
	}
	return $res;
}

sub request
{
# possible functions:
# modbus_write_registers($first, $num, $r1, $r2, $r3, $r4) (return true/false)
# @r = modbus_read_registers($first, $num) (return undef on error)
# @r = modbus_read_write_registers($roff, $rnum, $woff, $wnum, $r1, $r2, $r3, $r4)


# Zelio Input Registers:
# J1: bit register
#     bits 0-1:  1: set mint1, 3: set mint2
#     bit 12: query electro counter info
#     bit 13: query temp 1 or 2
#     bit 14: boiler
#     bit 15: alive
# J2: temp if J1b0 == 1

# Zelio Output Registers:
# O1: bit register
#     bit  0: termostat
#     bit  1: boiler
#     bit  2: valid mint1
#     bit  3: valid mint2
#     bit 15: alive
# O2: electro counter if J1b12 == 1
#     t1 if J1b12 == 0 and J1b13 =- 1
#     t2 if J1b12 == 0 and J1b13 == 0
# Q3: cycles since last electro cnt raise if J1b12 == 1
#     mint1 if J1b12 == 0 and J1b13 =- 1
#     mint2 if J1b12 == 0 and J1b13 == 0

	$alive = 0x8000;
	$q_electro = 0x1000;
	$q_temp1 = 0<<3;
	$q_temp2 = 1<<3;
	$q_electro = 2<<3;
	$q_output = 7<<3;
	# query ID, IE, IF, IG, saved bits unused, 3..6 <<3
	$set_output = 0x4;
	$boiler_set = 0x1;
	$set_mint1 = 0x1;
	$set_mint2 = 0x3;

	$boiler_out = 0x2;
	$termostat = 0x1;
	$valid_mint1 = 0x4;
	$valid_mint2 = 0x8;
	$valid_electro = 0x10;
	$by_invertor = 0x20;
	$by_el_input = 0x40;
	$phase1_miss = 0x80;
	$phase2_miss = 0x100;
	$phase3_miss = 0x200;
	$batt_low = 0x400;
	$valid_outbits = 1<<13;

	logwrite(4, "request");
	($prev, $prevus) = ($now, $nowus);
	($now, $nowus) = gettimeofday();
	foreach (keys %tempwvar) {
		if ($tempwtime{$_} <= $now) {
			logwrite(1, "Restored value $_ from $tempwvar{$_} to $wvar{$_}");
			delete($tempwvar{$_});
			delete($tempwtime{$_});
		}
	}
	modbus_write_registers(0+$wbase, 1, 0) || return undef;	# reset alive
	usleep($cycle*2000);
	modbus_write_registers(0+$wbase, 1, $alive|$q_electro) || return undef;
	usleep($cycle*2000);
	(($r, $el_cnt, $el_time) = modbus_read_registers(0+$rbase, 3)) || return undef;
	logwrite(5, "el_cnt: $el_cnt, el_time: $el_time");
	$el_time = 0 if $el_time < 0;
	$el_cnt += $el_cnt_base;
	if ($el_cnt < 0) {
		$el_cnt += 65536;
		$el_cnt_base += 65536;
	}
	if ($last_el_cnt > $el_cnt) {
		if ($el_cnt < 120) {
			# controller reloaded
			$el_cnt_base = $last_el_cnt = 0;
		}
		while ($last_el_cnt > $el_cnt) {
			$el_cnt_base += 65536;
			$el_cnt += 65536;
		}
	}
	if ($r & $valid_electro && $el_time) {
		set_var("electro_cnt", $el_cnt);
		$sum_el_cnt += $el_cnt - $last_el_cnt if defined($last_el_cnt);
		set_var("electro_pwr", int((3600*1000/800*(1000/$cycle))/$el_time+0.5)); # Watt
	}
	$last_el_cnt = $el_cnt;

	if (!($r & $valid_outbits)) {
		# controller reloaded?
		# set output regs (only boiler now)
		logwrite(0, "Not valid output regs on controller");
		$wboiler = (defined($tempwvar{"boiler"}) ? $tempwvar{"boiler"} : $wvar{"boiler"});
		modbus_write_registers(0+$wbase, 3, $alive, 0xffff, ($wboiler ? $boiler_set : 0)) || return undef;
		usleep($cycle*2000);
		modbus_write_registers(0+$wbase, 1, $set_output) || return undef;
		usleep($cycle*2000);
	}

	modbus_write_registers(0+$wbase, 1, $q_temp1) || return undef;
	usleep($cycle*2000);
	(($r, $t1, $mint1) = modbus_read_registers(0+$rbase, 3)) || return undef;
	if ($r & $alive) {
		logwrite(1, "Incorrect alive output");
		return undef;
	}
	logwrite(5, "t1: " . ($t1/10) . ", mint1: " . ($mint1/10) . ", termostat " . (($r & $termostat) ? "on" : "off"));
	set_var("t1", $t1/10);
	set_var("termostat", ($r & $termostat) ? 1 : 0);
	$rvar{"boiler_state"} = ($r & $boiler_out) ? 1 : 0;
	$rvar{"phase1"} = ($r & $phase1_miss) ? "fail" : "ok";
	$rvar{"phase2"} = ($r & $phase2_miss) ? "fail" : "ok";
	$rvar{"phase3"} = ($r & $phase3_miss) ? "fail" : "ok";
	$rvar{"by_el_input"} = ($r & $by_el_input) ? "on" : "off";
	$rvar{"by_invertor"} = ($r & $by_invertor) ? "on" : "off";
	$rvar{"low_batt"} = ($r & $batt_low) ? "low" : "good";
	logwrite(5, "phase1: " . ($r & $phase1_miss ? "off" : "on") . ", phase2: " . ($r & $phase2_miss ? "off" : "on") . ", phase3: " . ($r & $phase1_miss ? "off" : "on"));
	logwrite(5, "el_input: " . ($r & $by_el_input ? "on" : "off") . ", by_invertor: " . ($r & $by_invertor ? "on" : "off") . ", batt low: " . ($r & $batt_low ? "on" : "off"));

	$el_state = ($r & ($by_invertor | $by_el_input | $phase1_miss | $phase2_miss | $phase3_miss | $batt_low));
	if ($prev_el_state == $el_state && $el_state != $prev2_el_state) {
		el_changed($prev2_el_state, $el_state) if defined($prev2_el_state);
		$prev2_el_state = $el_state;
	}
	$prev_el_state = $el_state;
	if ($mint1 != int($wvar{"mint1"}*10+0.5) || !($r & $valid_mint1)) {
		# controller reloaded? set mint1
		logwrite(4, "set mint1 to " . int($wvar{"mint1"}*10+0.5));
		modbus_write_registers(0+$wbase, 2, $alive, int($wvar{"mint1"}*10+0.5)) || return undef;
		usleep($cycle*2000);
		modbus_write_registers(0+$wbase, 1, $set_mint1) || return undef;
		usleep($cycle*2000);
	}
	modbus_write_registers(0+$wbase, 1, $q_temp2) || return undef;
	usleep($cycle*2000);
	(($r, $t2, $mint2) = modbus_read_registers(0+$rbase, 3)) || return undef;
	if ($r & $alive) {
		logwrite(1, "Incorrect alive output");
		return undef;
	}
	logwrite(5, "t2: " . ($t2/10) . ", mint2: " . ($mint2/10) . ", boiler " . (($r & $boiler_out) ? "on" : "off"));
	set_var("t2", $t2/10);
	if ($mint2 != int($wvar{"mint2"}*10+0.5) || !($r & $valid_mint2)) {
		# controller reloaded? set mint2
		logwrite(4, "set mint2 to " . int($wvar{"mint2"}*10+0.5));
		modbus_write_registers(0+$wbase, 2, $alive, int($wvar{"mint2"}*10+0.5)) || return undef;
		usleep($cycle*2000);
		modbus_write_registers(0+$wbase, 1, $set_mint2) || return undef;
		usleep($cycle*2000);
	}
	$cur_boiler = ($r & $boiler_out) ? 1 : 0;
	if ($cur_boiler && $now>$prev && $now-$prev<60) {
		$sum{"boiler"} += $now-$prev;
	}
	$wboiler = (defined($tempwvar{"boiler"}) ? $tempwvar{"boiler"} : $wvar{"boiler"});
	if ($cur_boiler != $wboiler) {
		logwrite(0, "Incorrect boiler state, should be " . ($wboiler ? "on" : "off"));
		if ($boiler_state_alarm == 1) {
			sms_alarm("Incorrect boiler state");
			$boiler_state_alarm = 2;
		}
	} else {
		$boiler_state_alarm = 0;
	}
	# need to change boiler state?
	if ($now - $boiler_changed >= 30 || $boiler_state_alarm) {
		$wvar{"boiler"} = ($summer ? set_boiler_by_time() : set_boiler_by_temp());
		$wboiler = (defined($tempwvar{"boiler"}) ? $tempwvar{"boiler"} : $wvar{"boiler"});
		# every 5 mins, first part on, last part off
		# i.e if $wboiler=0.1, 12 secs on, then 108 secs off
		$sboiler = ($now % 300 < 300 * $wboiler ? 1 : 0);
		if ($sboiler != $cur_boiler) {
			logwrite(1, "boiler " . ($sboiler ? "on" : "off"));
			$boiler_changed = $now;
			modbus_write_registers(0+$wbase, 3, $alive, $boiler_set, $sboiler ? $boiler_set : 0) || return undef;
			usleep($cycle*2000);
			modbus_write_registers(0+$wbase, 1, $set_output) || return undef;
			usleep($cycle*2000);
		}
	}
	if (($prev - $prev % $period) != ($now - $now % $period)) {
		periodic();
	}
	logwrite(4, "request ok");
	return 1;
}

sub periodic
{
	logwrite(2, "call periodic()");
	@vars =  qw(t1 t2 electro_cnt electro_pwr termostat);
	foreach $var (@vars) {
		$sum{$var} += 0;
		$avg{$var} = $num{$var} ? $sum{$var}/$num{$var} : 0;
		$max{$var} = $cur_max{$var}+0;
		$min{$var} = $cur_min{$var}+0;
	}
	$period_time = (defined($prev_periodic) && $now > $prev_periodic) ? $now-$prev_periodic : $period;
	$avg{"electro_pwr"} = int($sum_el_cnt*3600/800*1000/$period_time+0.5); # Watt
	$avg{"boiler"} = int($sum{"boiler"}*100/$period+0.5);
	$avg{"termostat"} = int($avg{"termostat"}*100+0.5); # percents
	$prev_periodic = $now;
	$el_cnt = $last_el_cnt if !defined($el_cnt);
	$el_cnt += 0;
	# store to mysql
	connect_mysql();
	if ($dbh) {
		$table = "data";
		do_mysql("create table if not exists $table (
		           time timestamp not null default current_timestamp,
		           unixtime int unsigned not null,
		           t1 decimal(4, 1),
		           maxt1 decimal(4, 1),
		           mint1 decimal(4, 1),
		           comft1 decimal(4, 1),
		           t2 decimal(4, 1),
		           maxt2 decimal(4, 1),
		           mint2 decimal(4, 1),
		           comft2 decimal(4, 1),
		           termostat int unsigned,
		           el_cnt int unsigned,
		           el_pwr int unsigned,
		           max_el_pwr int unsigned,
		           boiler int unsigned,
		           index(time),
		           index(unixtime))");
		$wmint1 = (defined($tempwvar{"mint1"}) ? $tempwvar{"mint1"} : $wvar{"mint1"});
		$wmint2 = (defined($tempwvar{"mint2"}) ? $tempwvar{"mint2"} : $wvar{"mint2"});
		do_mysql("insert $table set unixtime=$now, " .
		         "                  t1=$avg{'t1'}, maxt1=$max{'t1'}, mint1=$min{'t1'}, comft1=$wmint1, " .
		         "                  t2=$avg{'t2'}, maxt2=$max{'t2'}, mint2=$min{'t2'}, comft2=$wmint2, " .
		         "                  termostat=$avg{'termostat'}, el_cnt = $el_cnt, " .
		         "                  el_pwr=$avg{'electro_pwr'}, max_el_pwr=$max{'electro_pwr'}, " .
		         "                  boiler=$avg{'boiler'}");
		disconnect_mysql();
	}
	foreach $var (@vars) {
		$sum{$var} = $num{$var} = 0;
		$cur_max{$var} = $cur_min{$var} = undef;
	}
	$sum{"boiler"} = 0;
	$sum_el_cnt = 0;
}

sub day
{
	my ($min, $hour, $now, $smin, $shour, $start, $emin, $ehour, $end, $interval);

	($min, $hour) = (localtime())[1, 2];
	foreach $interval (split(/[\s,]+/, $day)) {
		next unless $interval =~ /-/;
		($start, $end) = ($`, $');
		if ($start =~ /:/) {
			($shour, $smin) = ($`, $');
		} else {
			($shour, $smin) = ($start, 0);
		}
		if ($end =~ /:/) {
			($ehour, $emin) = ($`, $');
		} else {
			($ehour, $emin) = ($end, 0);
		}
		$start = $shour*60+$smin;
		$end = $ehour*60+$emin;
		$now = $hour*60+$min;
		if ($start > $end) {
			if ($now >= $start || $now < $end) {
				return 1;
			}
		} else {
			if ($now >= $start && $now < $end) {
				return 1;
			}
		}
	}
	return 0;
}

sub set_var
{
	my($var, $val) = @_;

	$sum{$var}+=$val;
	$num{$var}++;
	$rvar{$var}=$val;
	$cur_max{$var}=$val if (!defined($cur_max{$var}) || $cur_max{$var}<$val);
	$cur_min{$var}=$val if (!defined($cur_min{$var}) || $cur_min{$var}>$val);
}

sub sms_alarm
{
	my($text) = @_;

	open(MAIL, "| $sendmail") || return;
	print MAIL <<EOF;
From: <alarm\@happy.kiev.ua>
To: <sms-house\@happy.kiev.ua>

$text
EOF
	$text =~ s/\n$//s;
	$text =~ s/\n/; /gs;
	open(MAIL, "| $sendmail") || return;
	print MAIL <<EOF;
From: <alarm\@happy.kiev.ua>
To: <alarm-house\@happy.kiev.ua>
Subject: $text

EOF
	close(MAIL);
	logwrite(0, "Sent sms alarm: $text");
}

sub set_boiler_by_temp
{
	# Если в двух местах из трёх ниже комфортной - включаем
	my ($cnt);
	$cnt = 0;
	$cnt++ if $rvar{"t1"} < (defined($tempwvar{"mint1"}) ? $tempwvar{"mint1"} : $wvar{"mint1"});
	$cnt++ if $rvar{"t2"} < (defined($tempwvar{"mint2"}) ? $tempwvar{"mint2"} : $wvar{"mint2"});
	$cnt++ if $rvar{"termostat"};
	if (($rvar{"t1"} < 10 || $rvar{"t1"} > 27) && !$t1_alarmed) {
		sms_alarm("t1 " . $rvar{"t1"});
		$t1_alarmed = time();
	} elsif ($rvar{"t1"} > 12 && $rvar{"t1"} < 25) {
		$t1_alarmed = 0;
	}
	if (($rvar{"t2"} < 10 || $rvar{"t2"} > 27) && !$t2_alarmed) {
		sms_alarm("t2 " . $rvar{"t2"});
		$t2_alarmed = time();
	} elsif ($rvar{"t2"} > 12 && $rvar{"t2"} < 25) {
		$t2_alarmed = 0;
	}
	if ($t1>-40 || $t2>-40) {
		return ($cnt > 1 ? 1 : 0);
	} else {
		return $rvar{"termostat"};
	}
}

sub set_boiler_by_time
{
	return (day() ? 0.2 : 0);
}

sub el_changed
{
# $by_invertor | $by_el_input | $phase1_miss | $phase2_miss | $phase3_miss | $batt_low
	my ($prev_state, $new_state) = @_;
	my ($change_state, $report, $phases);

	$change_state = $prev_state ^ $new_state;
	$report = "";

	$phases = $phase1_miss | $phase2_miss | $phase3_miss;
	if (($chphases = ($change_state & $phases)) != 0) {
		if ($chphases == $phases) {
			$report = "All phases " . ($new_state & $phase1_miss ? "failed" : "restored") . "\n";
		} else {
			if ($chphases & $phase1_miss) {
				$report = "Phase1 " . ($new_state & $phase1_miss ? "failed" : "restored") . "\n";
			}
			if ($chphases & $phase2_miss) {
				$report .= "Phase2 " . ($new_state & $phase2_miss ? "failed" : "restored") . "\n";
			}
			if ($chphases & $phase3_miss) {
				$report .= "Phase3 " . ($new_state & $phase3_miss ? "failed" : "restored") . "\n";
			}
		}
	} else {	# ignoring changes in $by_inverter and $by_el_input if phases changed
		if (($prev_state & $by_invertor) && !($prev_state & $by_el_input) &&
		    !($new_state & $by_invertor) && ($new_state & $by_el_input) &&
		    (($new_state & $phases) == $phases)) {
			# changed from invertor to input - it's ok after phases restored
			logwrite(5, "Switched to input power");
		} else {
			if ($change_state & $by_invertor) {
				$report .= "Invertor input " . ($new_state & $by_invertor ? "on" : "off") . "\n";
			}
			if ($change_state & $by_el_input) {
				$report .= "Common input " . ($new_state & $by_el_input ? "on" : "off") . "\n";
			}
		}
	}
	if (($change_state & $batt_low) && ($new_state & $batt_low)) {
		$report .= "Low battary\n";
	}
	if ($report) {
		sms_alarm($report);
	}
}

sub connect_mysql
{
	if ($dbh = DBI->connect("DBI:mysql:$db:$mysql_host", $mysql_user, $mysql_passwd, { PrintError => 0 })) {
		$dbh->do("set names utf8");
	} else {
	        logwrite(0, "Can't connect to DB: $DBI::err ($DBI::errstr)\n");
	}
}

sub disconnect_mysql
{
	$dbh->disconnect() if defined($dbh);
	undef($dbh);
}

sub do_mysql
{
	my ($req) = @_;
	my ($rc);

	unless (($rc=$dbh->do($req))) {
	        $err="$DBI::err ($DBI::errstr)";
	        disconnect_mysql();
	        logwrite(0, "Database error: $err\n");
	}
	return $rc;
}

sub execute_mysql
{
        my ($req) = @_;
        my ($sth, $err);

        $sth = $dbh->prepare($req);
        unless ($sth->execute()) {
                $err="$DBI::err ($DBI::errstr)";
                $sth->finish();
                disconnect_mysql();
                logwrite(0, "Can't select: $err");
        }
        return $sth;
}

sub select_mysql
{
        my ($req) = @_;
        my ($sth, @res);

        $sth = execute_mysql($req);
        @res = $sth->fetchrow_array();
        $sth->finish();
        return @res;
}

