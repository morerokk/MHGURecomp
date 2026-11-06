package main

import (
	"flag"
	"log"
)

func main() {
	cfg := DefaultConfig()
	flag.StringVar(&cfg.Addr, "addr", cfg.Addr, "address and port to listen on (TCP and UDP)")
	flag.IntVar(&cfg.MaxConns, "max-conns", cfg.MaxConns, "connections at once")
	flag.IntVar(&cfg.MaxConnsPerIP, "max-conns-per-ip", cfg.MaxConnsPerIP, "connections at once from one IP address")
	flag.IntVar(&cfg.MaxHubs, "max-hubs", cfg.MaxHubs, "hubs at once")
	flag.IntVar(&cfg.MaxHubsPerIP, "max-hubs-per-ip", cfg.MaxHubsPerIP, "hubs at once from one IP address")
	flag.IntVar(&cfg.ListMax, "list-max", cfg.ListMax, "hubs in one search answer (a random choice when there are more)")
	flag.Float64Var(&cfg.Bandwidth, "bandwidth", cfg.Bandwidth, "bytes a second per player link and direction")
	minProto := flag.Uint("min-proto", uint(cfg.MinProto), "oldest link protocol (the game's PROTO_VERSION) served; older games are refused (0: any)")
	flag.BoolVar(&cfg.SeparatePhones, "separate-phones", cfg.SeparatePhones, "public hubs of Android players among Android players only, as the game asks (-separate-phones=false: one public room for all)")
	flag.BoolVar(&cfg.Verbose, "v", false, "log hubs and joins (never IP addresses)")
	flag.Parse()
	if *minProto > 0xFFFF {
		log.Fatalf("-min-proto %d: at most 65535", *minProto)
	}
	cfg.MinProto = uint16(*minProto)
	s := NewServer(cfg)
	if err := s.Listen(); err != nil {
		log.Fatal(err)
	}
	log.Printf("MHGURecomp hub server on %s (TCP and UDP)", s.Addr())
	if cfg.MinProto > 0 {
		log.Printf("refusing games older than link protocol %d", cfg.MinProto)
	}
	if !cfg.SeparatePhones {
		log.Printf("one public room: Android players' public hubs are listed with everyone's")
	}
	log.Fatal(s.Serve())
}
