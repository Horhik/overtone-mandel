// Physarum (slime mould) agents on a trail map, CPU. Jeff Jones' model: sense ahead, turn to the
// strongest trail, move, deposit; the trail diffuses and decays.
#ifndef PHYSARUM_H
#define PHYSARUM_H

typedef struct {
    float sensorAngle;   // radians, between the three sensors
    float sensorDist;    // cells
    float turn;          // radians per step
    float speed;         // cells per step
    float deposit;       // trail added per agent per step
    float decay;         // trail multiplier per step (0..1)
    float jolt;          // 0..1: fraction of agents given a random heading this step
} PhysParams;

void PhysInit(int w, int h, int agents, unsigned seed);
void PhysStep(PhysParams p);
const unsigned char *PhysTrail8(void);   // w*h grayscale, for UpdateTexture
int  PhysW(void);
int  PhysH(void);

#endif
