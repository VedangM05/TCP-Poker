# CP

Competitive programming snippets and simple client-server example.

Algorithms:
1. MONTE-CARLO SIMULATION : Runs simulation on random draws (set to 2000 simulations).
2. POT ODDS CALCULATION : Compares RISK and REWARD.
3. RULE BASED LOGIC : Decides to CALL or FOLD.
4. OPPONENT MODELLING : Tracks your play style over time (after 10 hands).
    1. If you are Tight (play few hands), the AI assumes your bets are strong, so it increases its requiredEquity (it needs a better hand to call you).
    2. If you are Aggressive, the AI assumes you could be bluffing, so it decreases its requiredEquity (it's more willing to call you down).
5. RANDOM BLUFFS :
    1.  Pure Bluff: If you check to the bot on the turn or river, it has a 10% chance to make a half-pot bet, even with a terrible hand.
    2. Semi-Bluff: If the bot has a strong draw (flush/straight draw) and you bet, it has a 20% chance to raise you instead of just calling, to try and win the pot immediately.


This repository contains:

- `client` - directory for client source (if present)
- `server` - directory for server source
- `client.cpp`, `server.cpp` - example C++ source files

How to build (macOS / Linux):

```sh
g++ -o server server.cpp
g++ -o client client.cpp
./server &
./client
```
