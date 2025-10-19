#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <atomic> // For myTurn

#define PORT 5555

int g_sock = 0;
std::atomic<bool> g_myTurn{false};
std::vector<std::string> g_holeCards;
std::vector<std::string> g_communityCards;


// ===== REWRITTEN: Utility to Display Cards =====
// This version now translates ASCII letters back to Unicode symbols
std::string displayCards(const std::vector<std::string> &cards) {
    std::stringstream ss;
    for (int line = 0; line < 5; line++) {
        for (const auto &c_str : cards) {
            if (c_str.empty()) continue;
            
            std::string rank = c_str.substr(0, c_str.size() - 1);
            std::string suit_letter(1, c_str.back()); 

            if (line == 0) ss << "┌─────┐ ";
            else if (line == 1) ss << "│" << rank << (rank.size() == 1 ? "    │ " : "   │ ");
            else if (line == 2) {
                // NEW: Translate ASCII suit to Unicode for client display
                std::string displaySuit = "?";
                if (suit_letter == "H") displaySuit = "♥";
                else if (suit_letter == "D") displaySuit = "♦";
                else if (suit_letter == "C") displaySuit = "♣";
                else if (suit_letter == "S") displaySuit = "♠";
                ss << "│  " << displaySuit << "  │ ";
            }
            else if (line == 3) ss << "│" << (rank.size() == 1 ? "    " : "   ") << rank << "│ ";
            else ss << "└─────┘ ";
        }
        ss << "\n";
    }
    return ss.str();
}

// ===== REWRITTEN: Receive Thread =====
// ===== REWRITTEN: Receive Thread =====
void receiveMessages() {
    char buffer[1024];
    std::string networkBuffer = ""; 
    
    while(true) {
        int valread = read(g_sock, buffer, 1023); 
        if(valread <= 0) {
            std::cout << "Disconnected from server." << std::endl;
            g_myTurn = false;
            close(g_sock);
            exit(0);
        }
        buffer[valread] = '\0';
        networkBuffer += buffer;

        size_t pos;
        while ((pos = networkBuffer.find('\n')) != std::string::npos) {
            std::string msg = networkBuffer.substr(0, pos);
            networkBuffer.erase(0, pos + 1);

            // --- Process the clean message ---
            
            if (msg.find("GAME_STARTING") != std::string::npos) {
                // NEW: Clear old cards and announce the new hand
                g_communityCards.clear();
                std::cout << "\n-------------------------------\n";
                std::cout << "--- NEW HAND STARTING ---\n" << std::endl;
            }
            else if (msg.find("YOUR_MOVE") != std::string::npos) {
                std::cout << "\n>>> YOUR TURN TO ACT <<<" << std::endl;
                g_myTurn = true;
            }
            else if (msg.find("HOLE ") == 0) {
                g_holeCards.clear();
                std::string rest = msg.substr(5);
                std::stringstream ss_cards(rest);
                std::string card;
                while (ss_cards >> card) {
                    g_holeCards.push_back(card);
                }
                std::cout << "--- Your Hole Cards ---" << std::endl;
                std::cout << displayCards(g_holeCards) << std::endl;
            }
            else if (msg.find("CARDS ") == 0) {
                g_communityCards.clear();
                std::string rest = msg.substr(6);
                std::stringstream ss_cards(rest);
                std::string card;
                while (ss_cards >> card) {
                    g_communityCards.push_back(card);
                }
                
                // --- FIX: Display cards on separate lines ---
                if (g_communityCards.size() == 3) {
                    std::cout << "\n--- FLOP ---";
                } else if (g_communityCards.size() == 4) {
                    std::cout << "\n--- TURN ---";
                } else if (g_communityCards.size() == 5) {
                    std::cout << "\n--- RIVER ---";
                }

                // Display hand
                std::cout << "\n--- Your Hand ---" << std::endl;
                std::cout << displayCards(g_holeCards);
                
                // Display community cards
                std::cout << "--- Community Cards ---" << std::endl;
                std::cout << displayCards(g_communityCards) << std::endl;
            }
            else if (msg.find("CHAT:") == 0) {
                std::string chat = msg.substr(5);
                size_t colonPos = chat.find(':');
                if (colonPos != std::string::npos) {
                    std::string name = chat.substr(0, colonPos);
                    std::string chatMsg = chat.substr(colonPos + 1);
                    std::cout << "[" << name << "]: " << chatMsg << std::endl;
                }
            }
            else {
                // Print all other server messages
                std::cout << msg << std::endl;
            }
        }
    }
}// ===== Main (REWRITTEN Input Loop) =====
int main() {
    std::string serverIP, playerName;
    std::cout << "Enter server IP (e.g., 127.0.0.1): ";
    std::getline(std::cin, serverIP);
    std::cout << "Enter your player name: ";
    std::getline(std::cin, playerName);

    struct sockaddr_in serv_addr;
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, serverIP.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cout << "Invalid address.\n"; return -1;
    }
    if (connect(g_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cout << "Connection failed.\n"; return -1;
    }

    send(g_sock, (playerName + "\n").c_str(), playerName.size() + 1, 0);

    std::thread recvThread(receiveMessages);
    recvThread.detach();

    std::cout << "Connected! Waiting for game to start..." << std::endl;
    std::cout << "Type '/chat <msg>' to chat." << std::endl;
    std::cout << "Type 'FOLD', 'CALL', 'CHECK', or 'RAISE <amount>' when it's your turn." << std::endl;

    std::string input;
    while(std::getline(std::cin, input)) {
        if(input.empty()) continue;

        if (input.find("/chat ") == 0) {
            std::string chatMsg = "CHAT:" + input.substr(6) + "\n";
            send(g_sock, chatMsg.c_str(), chatMsg.size(), 0);
        }
        else if (g_myTurn) {
            std::string upperInput = input;
            std::transform(upperInput.begin(), upperInput.end(), upperInput.begin(), ::toupper);

            if (upperInput.find("FOLD") != 0 && upperInput.find("CALL") != 0 &&
                upperInput.find("RAISE") != 0 && upperInput.find("CHECK") != 0) {
                std::cout << "Invalid command. Use FOLD, CALL, CHECK, or RAISE <amount>." << std::endl;
            } else {
                send(g_sock, (input + "\n").c_str(), input.size() + 1, 0);
                g_myTurn = false;
            }
        }
        else {
            std::cout << "It's not your turn to make a move." << std::endl;
        }
    }

    close(g_sock);
    return 0;
}