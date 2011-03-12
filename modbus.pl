
$sendmail = "/usr/sbin/sendmail -obb -t -oem";
$day = "17-00:30, 4-6";
$period = 300;
$cycle = 10; # ms
$mysql_host = "localhost";
$mysql_user = "zelio";
$mysql_passwd = "********";
$db = "zelio";
$rbase = 24;

use Time::HiRes "gettimeofday";
use DBI;

sub init
{
	%rvar = (
		"t1" => "",
		"t2" => "",
		"termostat" => 0,
		"electro_cnt" => "");
	%wvar = (
		"mint1" => "22",
		"mint2" => "21.5",
		"boiler" => "0");
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
			$i=0;
			$res = "";
			foreach (sort keys %rvar) {
				$res .= (++$i < keys %rvar) ? "r" : "R";
				if ($cmd eq "getavg" && defined($avg{$_})) {
					$res .= " $_: $avg{$_}\n";
				} else {
					$res .= " $_: $rvar{$_}\n";
				}
			}
		} elsif ($cmd eq "getavg" && defined($avg{$var})) {
			$res = "R $var: $avg{$var}\n";
		} elsif ($rvar{$var}) {
			$res = "R $var: $rvar{$var}\n";
		} else {
			$res = "E Unknown variable $var\n";
		}
	} elsif ($command =~ /^set\s+([a-z0-9_]+)=(\S+)\s*$/i) {
		($var, $val) = ($1, $2);
		if (!defined($wvar{$var})) {
			$res = "E Unknown variable $var\n";
		} else {
			$wvar{$var} = $val;
		}
		# write registers
		# ...
	} elsif ($command =~ /^(q|quit|bye)\s*$/i) {
		$res = "R bye\n";
		$bye = 1;
	} else {
		$res = "E What?\n";
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
	$boiler_out = 0x2;
	$termostat = 0x1;
	$set_mint1 = 0x1;
	$set_mint2 = 0x3;
	$valid_mint1 = 0x4;
	$valid_mint2 = 0x8;
	$valid_electro = 0x10;
	$valid_outbits = 1<<13;

	logwrite(4, "request");
	($prev, $prevus) = ($now, $nowus);
	($now, $nowmus) = gettimeofday();
	modbus_write_registers(0, 1, 0) || return undef;	# reset alive
	modbus_write_registers(0, 1, $alive|$q_electro) || return undef;
	(($r, $el_cnt, $el_time) = modbus_read_registers(0+$rbase, 3)) || return undef;
	#logwrite(2, "el_cnt: $el_cnt, el_time: $el_time");
	$el_cnt += $el_cnt_base;
	if ($last_cnt > $el_cnt) {
		if ($last_cnt < $el_cnt+65536 && $last_cnt > $el_cnt+65536+5) {
			$el_cnt_base += 65536;
			$el_cnt += 65536;
		}
	}
	if ($r & $valid_electro && $el_time) {
		set_var("electro_cnt", $el_cnt-$last_cnt);
		set_var("electro_pwr", (3600/800*5000)/$el_time); # Watt
	}

	if (!($r & $valid_outbits)) {
		# controller reloaded?
		# set output regs (only boiler now)
		logwrite(0, "Not valid output regs on controller");
		modbus_write_registers(0, 3, $set_output, 0xffff, ($wvar{"boiler"} ? $boiler_set : 0)) || return undef;
	}

	modbus_write_registers(0, 1, $q_temp1) || return undef;
	(($r, $t1, $mint1) = modbus_read_registers(0+$rbase, 3)) || return undef;
	if ($r & $alive) {
		logwrite(1, "Incorrect alive output");
		return undef;
	}
	#logwrite(2, "t1: " . ($t1/10) . ", mint1: " . ($mint1/10) . ", termostat " . (($r & $termostat) ? "on" : "off"));
	set_var("t1", $t1/10);
	set_var("termostat", ($r & $termostat) ? 1 : 0);
	$rvar{"boiler"} = ($r & $boiler_out) ? 1 : 0;
#	if ($r & $valid_mint1) {
#		$rvar{"mint1"} = $wvar{"mint1"} = $mint1/10;
#	} else {
	if ($mint1 != int($wvar{"mint1"}*10+0.5) || !($r & $valid_mint1)) {
		# controller reloaded? set mint1
		#logwrite(2, "set mint1 to " . int($wvar{"mint1"}*10+0.5));
		modbus_write_registers(0, 2, $set_mint1, int($wvar{"mint1"}*10+0.5)) || return undef;
	}
	modbus_write_registers(0, 1, $q_temp2) || return undef;
	(($r, $t2, $mint2) = modbus_read_registers(0+$rbase, 3)) || return undef;
	if ($r & $alive) {
		logwrite(1, "Incorrect alive output");
		return undef;
	}
	#logwrite(2, "t2: " . ($t2/10) . ", mint2: " . ($mint2/10) . ", boiler " . (($r & $boiler_out) ? "on" : "off"));
	set_var("t2", $t2/10);
#	if ($r & $valid_mint2) {
#		$rvar{"mint2"} = $wvar{"mint2"} = $mint2/10;
#	} else {
	if ($mint2 != int($wvar{"mint2"}*10+0.5) || !($r & $valid_mint2)) {
		# controller reloaded? set mint2
		#logwrite(2, "set mint2 to " . int($wvar{"mint2"}*10+0.5));
		modbus_write_registers(0, 2, $set_mint2, int($wvar{"mint2"}*10+0.5)) || return undef;
	}
	$cur_boiler = ($r & $boiler_out) ? 1 : 0;
	if ($cur_boiler && $now>$prev && $now-$prev<60) {
		$sum{"boiler"} += $now-$prev;
	}
	if ($cur_boiler != $wvar{"boiler"}) {
		logwrite(0, "Incorrect boiler state, should be " . ($wvar{"boiler"} ? "on" : "off"));
		if ($boiler_state_alarm == 1) {
			sms_alarm("Incorrect boiler state");
			$boiler_state_alarm = 2;
		}
	} else {
		$boiler_state_alarm = 0;
	}
	# need to change boiler state?
	$wvar{"boiler"} = set_boiler();
	if ($wvar{"boiler"} != $cur_boiler) {
		logwrite(1, "boiler " . ($wvar{"boiler"} ? "on" : "off"));
		modbus_write_registers(0, 3, $set_output, $boiler_set, $wvar{"boiler"} ? $boiler_set : 0) ||
			return undef;
	}
	if (($prev - $prev % $period) != ($now - $now % $period)) {
		periodic();
	}
	logwrite(4, "request ok");
	return 1;
}

sub periodic
{
	@vars =  qw(t1 t2 electro_cnt electro_pwr);
	foreach $var (@vars) {
		$avg{$var} = $num{$var} ? $sum{$var}/$num{$var} : undef;
		$max{$var} = $cur_max{$var};
		$min{$var} = $cur_min{$var};
	}
	$avg{"electro_pwr"} = $sum{"electro_cnt"}*3600/800*1000/$period; # Watt
	$avg{"boiler"} = int($sum{"boiler"}*100/$period+0.5);
	$avg{"termostat"} = int($avg{"termostat"}*100+0.5); # percents
	# store to mysql
	connect_mysql();
	if ($dbh) {
		$table = "data";
		do_mysql("create table if not exist $table (
		           time timestamp not null default current_timestamp,
		           unixtime int unsigned not null,
		           t1 decimal(4, 1),
		           maxt1 decimal(4, 1),
		           mint1 decimal(4, 1),
		           t2 decimal(4, 1),
		           maxt2 decimal(4, 1),
		           mint2 decimal(4, 1),
		           termostat int unsigned,
		           el_cnt int unsigned,
		           el_pwr int unsigned,
		           max_el_pwr int unsigned,
		           termostat int unsigned,
		           boiler int unsigned,
		           index(timestamp),
		           index(unixtime))");
		do_mysql("insert $table set unixtime=$now, " .
		         "                  t1=$avg{'t1'}, maxt1=$max{'t1'}, mint1=$min{'t1'}, " .
		         "                  t2=$avg{'t2'}, maxt1=$max{'t2'}, mint1=$min{'t2'}, " .
		         "                  termostat=$rvar{'termostat'}, el_cnt = $sum{'electro_cnt'}, " .
		         "                  el_pwr=$avg{'electro_pwr'}, max_el_pwr=max{'electro_pwr'}, " .
		         "                  termostat=$avg{'termostat'}, boiler=$avg{'boiler'}");
		disconnect_mysql();
	}
	foreach $var (@vars) {
		$sum{$var} = $num{$var} = 0;
		$cur_max{$var} = $cur_min{$var} = undef;
	}
	$sum{"boiler"} = 0;
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
	$cur_max{$var}=$var if !defined($cur_max{$var}) || $cur_max{$var}<$var;
	$cur_min{$var}=$var if !defined($cur_min{$var}) || $cur_min{$var}>$var;
}

sub sms_alarm
{
	my($text) = @_;

	open(MAIL, "| $sendmail") || return;
	print MAIL <<EOF;
From: <alarm\@happy.kiev.ua>
To: <sms-dom\@happy.kiev.ua>

$text
EOF
	close(MAIL);
}

sub set_boiler
{
	# Если в двух местах из трёх ниже комфортной - включаем
	my ($cnt);
	$cnt = 0;
	$cnt++ if $rvar{"t1"} < $wvar{"mint1"};
	$cnt++ if $rvar{"t2"} < $wvar{"mint2"};
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

