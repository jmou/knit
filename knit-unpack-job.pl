#!/usr/bin/perl

use v5.12;
use File::Basename;

my $usage = "usage: $0 <job> <dir>\n";

my ($scratch, $job, $dir) = @ARGV or die $usage;
$scratch == '--scratch' or die $usage;

mkdir $dir or die $!;
mkdir "$dir/work" or die $!;
mkdir "$dir/work/out" or die $!;
mkdir "$dir/out.knit" or die $!;

open my $fh, "knit-cat-file -p $job |" or die $!;
while (<$fh>) {
    chomp;
    my ($digest, $path) = split(/\t/, $_, 2);
    die unless $path;
    system('mkdir', '-p', dirname "$dir/work/in/$path") == 0 or die $?;
    open STDOUT, '>', "$dir/work/in/$path" or die $!;
    system('knit-cat-file', 'resource', $digest) == 0 or die $?;
}
close $fh or die $!;
