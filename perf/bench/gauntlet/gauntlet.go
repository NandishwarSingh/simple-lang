package main

import (
	"fmt"
	"strconv"
)

func rngStep(s uint64) uint64 {
	return s*6364136223846793005 + 1442695040888963407
}
func rngVal(s uint64) int64 { return int64(s >> 33) }

type Body struct{ x, y, z, vx, vy, vz int64 }

func interact(a, b Body) Body {
	dx, dy, dz := b.x-a.x, b.y-a.y, b.z-a.z
	d2 := dx*dx + dy*dy + dz*dz + 1
	inv := int64(1000000) / (d2/1000 + 1)
	r := a
	r.vx = a.vx + (dx*inv)/100000
	r.vy = a.vy + (dy*inv)/100000
	r.vz = a.vz + (dz*inv)/100000
	return r
}

func phaseA() int64 {
	var bs [32]Body
	var r uint64 = 88172645463325252
	for i := 0; i < 32; i++ {
		r = rngStep(r)
		x := rngVal(r) % 1000
		r = rngStep(r)
		y := rngVal(r) % 1000
		r = rngStep(r)
		z := rngVal(r) % 1000
		bs[i] = Body{x, y, z, 0, 0, 0}
	}
	for step := 0; step < 30000; step++ {
		for i := 0; i < 32; i++ {
			for j := 0; j < 32; j++ {
				if i != j {
					bs[i] = interact(bs[i], bs[j])
				}
			}
		}
	}
	var sum int64 = 0
	for i := 0; i < 32; i++ {
		sum += bs[i].vx + bs[i].vy + bs[i].vz
	}
	return sum
}

func phaseB() uint64 {
	items := make([]string, 0, 500000)
	for k := 0; k < 500000; k++ {
		items = append(items, "k"+strconv.Itoa(k)+"_"+strconv.Itoa((k*13)%97))
	}
	var acc uint64 = 0
	for _, s := range items {
		var h uint64 = 14695981039346656037
		for c := 0; c < len(s); c++ {
			h = (h ^ uint64(s[c])) * 1099511628211
		}
		acc ^= h
	}
	return acc + uint64(len(items))
}

func phaseC() int64 {
	var a [25000]int64
	var r uint64 = 99887766554433
	for i := 0; i < 25000; i++ {
		r = rngStep(r)
		a[i] = rngVal(r) % 1000000
	}
	for i := 1; i < 25000; i++ {
		v := a[i]
		j := i - 1
		for j >= 0 && a[j] > v {
			a[j+1] = a[j]
			j--
		}
		a[j+1] = v
	}
	var s int64 = 0
	for i := 0; i < 25000; i++ {
		s += a[i] * int64(i)
	}
	return s
}

func fib(n int64) int64 {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}
func phaseD() int64 { return fib(40) }

func phaseE() int64 {
	var a, b, c [100][100]int64
	for i := 0; i < 100; i++ {
		for j := 0; j < 100; j++ {
			a[i][j] = int64((i*7 + j) % 13)
			b[i][j] = int64((i*5 + j*3) % 11)
		}
	}
	var acc int64 = 0
	for rep := 0; rep < 150; rep++ {
		c = [100][100]int64{}
		for i := 0; i < 100; i++ {
			for k := 0; k < 100; k++ {
				aik := a[i][k]
				for j := 0; j < 100; j++ {
					c[i][j] += aik * b[k][j]
				}
			}
		}
		for i := 0; i < 100; i++ {
			acc += c[i][i]
		}
	}
	return acc
}

func main() {
	a := phaseA()
	b := phaseB()
	c := phaseC()
	d := phaseD()
	e := phaseE()
	var sum uint64 = 0
	sum += uint64(a)
	sum ^= b
	sum += uint64(c)
	sum ^= uint64(d)
	sum += uint64(e)
	fmt.Println(sum % 1000000000000)
}
