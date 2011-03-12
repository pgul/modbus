
sub init
{
	logwrite(1, "init");
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
	logwrite(1, "request");
	modbus_write_registers(0, 3, 111, 222, 333) || return undef;
#	(($r1, $r2, $r3, $r4) = modbus_read_registers(24, 4)) || return undef;
	for ($i=0; $i<50; $i++) {
		($r) = modbus_read_registers($i, 1);
		logwrite(1, "Read register $i: $r!") if defined($r);
	}
	#logwrite(1, "request ok: $r1, $r2, $r3, $r4");
	return 1;
}

