package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
)

func main() {
	cfg := DefaultConfig()
	flag.StringVar(&cfg.Addr, "addr", cfg.Addr, "address and port to listen on (TCP and UDP)")
	flag.IntVar(&cfg.MaxConns, "max-conns", cfg.MaxConns, "connections at once")
	flag.IntVar(&cfg.MaxConnsPerIP, "max-conns-per-ip", cfg.MaxConnsPerIP, "connections at once from one IP address")
	flag.IntVar(&cfg.MaxHubs, "max-hubs", cfg.MaxHubs, "hubs at once")
	flag.IntVar(&cfg.MaxHubsPerIP, "max-hubs-per-ip", cfg.MaxHubsPerIP, "hubs at once from one IP address")
	flag.IntVar(&cfg.ListMax, "list-max", cfg.ListMax, "hubs in one search answer (a random choice when there are more)")
	flag.IntVar(&cfg.ListSearchMax, "list-search-max", cfg.ListSearchMax, "Hunter Search networks in one search answer, after the hubs (a random choice when there are more)")
	flag.Float64Var(&cfg.Bandwidth, "bandwidth", cfg.Bandwidth, "bytes a second per player link and direction")
	flag.IntVar(&cfg.MaxPresence, "max-presence", cfg.MaxPresence, "players online at once that the friends list keeps track of (protocol 4)")
	minProto := flag.Uint("min-proto", uint(cfg.MinProto), "oldest link protocol (the game's PROTO_VERSION) served; older games are refused (0: any)")
	maxProto := flag.Uint("max-proto", uint(cfg.MaxProto), "newest link protocol served; newer ones are refused (0: any)")
	flag.BoolVar(&cfg.SeparatePhones, "separate-phones", cfg.SeparatePhones, "public hubs of Android players among Android players only, as the game asks (-separate-phones=false: one public room for all)")
	settings := flag.String("settings", "", "file with min-proto, max-proto and separate-phones, read at the start and again on SIGHUP (systemctl reload): they change without a restart; the file wins over the flags")
	flag.BoolVar(&cfg.Verbose, "v", false, "log hubs and joins (never IP addresses)")
	flag.Parse()
	if *minProto > 0xFFFF {
		log.Fatalf("-min-proto %d: at most 65535", *minProto)
	}
	if *maxProto > 0xFFFF {
		log.Fatalf("-max-proto %d: at most 65535", *maxProto)
	}
	cfg.MinProto, cfg.MaxProto = uint16(*minProto), uint16(*maxProto)
	flags := Settings{cfg.MinProto, cfg.MaxProto, cfg.SeparatePhones}
	if *settings != "" {
		st, err := readSettings(*settings, flags)
		if err != nil {
			log.Fatal(err)
		}
		cfg.MinProto, cfg.MaxProto, cfg.SeparatePhones = st.MinProto, st.MaxProto, st.SeparatePhones
	}
	if err := checkProtos(Settings{cfg.MinProto, cfg.MaxProto, cfg.SeparatePhones}); err != nil {
		log.Fatal(err)
	}
	s := NewServer(cfg)
	if err := s.Listen(); err != nil {
		log.Fatal(err)
	}
	log.Printf("MHGURecomp hub server on %s (TCP and UDP)", s.Addr())
	if cfg.MinProto > 0 {
		log.Printf("refusing games older than link protocol %d", cfg.MinProto)
	}
	if cfg.MaxProto > 0 {
		log.Printf("refusing link protocols newer than %d", cfg.MaxProto)
	}
	if !cfg.SeparatePhones {
		log.Printf("one public room: Android players' public hubs are listed with everyone's")
	}
	if *settings != "" {
		go reloadOnHangup(s, *settings, flags)
	}
	log.Fatal(s.Serve())
}

// SIGHUP: the settings file again, applied without a restart (Server.Apply). A file that can't be read or has a
// mistake changes nothing. (Windows has no SIGHUP: there the file is only read at the start.)
func reloadOnHangup(s *Server, path string, flags Settings) {
	hup := make(chan os.Signal, 1)
	signal.Notify(hup, syscall.SIGHUP)
	for range hup {
		st, err := readSettings(path, flags)
		switch {
		case err != nil:
			log.Printf("settings unchanged: %v", err)
		case st == s.Settings():
			log.Printf("settings unchanged: %s has the same ones (min-proto %d, max-proto %d, separate-phones %v)", path,
				st.MinProto, st.MaxProto, st.SeparatePhones)
		default:
			s.Apply(st)
		}
	}
}

// checkProtos: a max-proto below min-proto would refuse every game.
func checkProtos(st Settings) error {
	if st.MaxProto != 0 && st.MaxProto < st.MinProto {
		return fmt.Errorf("max-proto %d is below min-proto %d: every game would be refused", st.MaxProto, st.MinProto)
	}
	return nil
}

// readSettings reads "key = value" lines ('#' starts a comment): min-proto and max-proto (numbers, 0: any) and
// separate-phones (on / off). A key the file leaves out keeps the flag's value (flags). An unknown key, a bad value or
// a max-proto below min-proto is an error.
func readSettings(path string, flags Settings) (Settings, error) {
	f, err := os.Open(path)
	if err != nil {
		return flags, err
	}
	defer f.Close()
	st := flags
	sc := bufio.NewScanner(f)
	for n := 1; sc.Scan(); n++ {
		line, _, _ := strings.Cut(sc.Text(), "#")
		k, v, ok := strings.Cut(line, "=")
		k, v = strings.ToLower(strings.TrimSpace(k)), strings.ToLower(strings.TrimSpace(v))
		if !ok && k == "" {
			continue
		}
		bad := fmt.Errorf("%s line %d: %q (min-proto = <number>, max-proto = <number> or separate-phones = on / off)",
			path, n, strings.TrimSpace(line))
		switch k {
		case "min-proto", "max-proto":
			p, err := strconv.ParseUint(v, 10, 16)
			if !ok || err != nil {
				return flags, bad
			}
			if k == "min-proto" {
				st.MinProto = uint16(p)
			} else {
				st.MaxProto = uint16(p)
			}
		case "separate-phones":
			switch v {
			case "on", "true", "yes", "1":
				st.SeparatePhones = true
			case "off", "false", "no", "0":
				st.SeparatePhones = false
			default:
				return flags, bad
			}
		default:
			return flags, bad
		}
	}
	if err := sc.Err(); err != nil {
		return flags, err
	}
	if err := checkProtos(st); err != nil {
		return flags, fmt.Errorf("%s: %v", path, err)
	}
	return st, nil
}
