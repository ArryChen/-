#include "../include/combustion_chamber.h"

#include "../include/constants.h"
#include "../include/units.h"
#include "../include/piston.h"
#include "../include/connecting_rod.h"
#include "../include/utilities.h"
#include "../include/exhaust_system.h"
#include "../include/cylinder_bank.h"
#include "../include/engine.h"

#include <cmath>

CombustionChamber::CombustionChamber() {
    m_crankcasePressure = 0.0;
    m_piston = nullptr;
    m_head = nullptr;
    m_engine = nullptr;
    m_fuel = nullptr;
    m_lit = false;
    m_litLastFrame = false;
    m_peakTemperature = 0;

    m_meanPistonSpeedToTurbulence = nullptr;
    m_nBurntFuel = 0;

    m_lastTimestepTotalExhaustFlow = 0;
    m_exhaustFlow = 0;
    m_exhaustFlowRate = 0;
    m_intakeFlowRate = 0;

    m_exhaustMomentum = 0.0;
}

CombustionChamber::~CombustionChamber() {
    /* void */
}

void CombustionChamber::initialize(const Parameters &params) {
    m_piston = params.Piston;
    m_head = params.Head;
    m_fuel = params.Fuel;
    m_crankcasePressure = params.CrankcasePressure;
    m_meanPistonSpeedToTurbulence = params.MeanPistonSpeedToTurbulence;

    m_pistonSpeed = new double[StateSamples];
    m_pressure = new double[StateSamples];
    for (int i = 0; i < StateSamples; ++i) {
        m_pistonSpeed[i] = 0;
        m_pressure[i] = 0;
    }

    m_exhaustRunner.initialize(
        units::pressure(1.0, units::atm),
        units::volume(100.0, units::cc),
        units::celcius(25.0));
}

double CombustionChamber::getVolume() const {
    const double combustionPortVolume = m_head->getCombustionChamberVolume();
    const CylinderBank *bank = m_head->getCylinderBank();

    const double area = bank->boreSurfaceArea();
    const double s =
        m_piston->relativeX() * bank->getDx()
        + m_piston->relativeY() * bank->getDy();
    const double displacement =
        area * (bank->getDeckHeight() - s - m_piston->getCompressionHeight());

    return displacement + combustionPortVolume;
}

double CombustionChamber::pistonSpeed() const {
    const CylinderBank *bank = m_head->getCylinderBank();
    return
        m_piston->m_body.v_x * bank->getDx()
        + m_piston->m_body.v_y * bank->getDy();
}

double CombustionChamber::calculateMeanPistonSpeed() const {
    double avg = 0;
    for (int i = 0; i < StateSamples; ++i) {
        avg += m_pistonSpeed[i];
    }

    avg /= StateSamples;
    return avg;
}

double CombustionChamber::calculateFiringPressure() const {
    double firingPressure = 0;
    for (int i = 0; i < StateSamples; ++i) {
        if (m_pressure[i] > firingPressure) {
            firingPressure = m_pressure[i];
        }
    }

    return firingPressure;
}

bool CombustionChamber::popLitLastFrame() {
    const bool lit = m_litLastFrame;
    m_litLastFrame = false;

    return lit;
}

void CombustionChamber::ignite() {
    if (!m_lit) {
        if (m_system.mix().p_fuel == 0) return;

        const double afr = m_system.mix().p_o2 / m_system.mix().p_fuel;
        const double equivalenceRatio = afr / m_fuel->getMolecularAfr();
        if (equivalenceRatio < 0.5) return;
        else if (equivalenceRatio > 1.9) return;

        // temp
        //equivalenceRatio = std::fmin(7.5, std::fmax(20.5, equivalenceRatio));
        //equivalenceRatio = 15.125;

        m_flameEvent.lastVolume = getVolume();
        m_flameEvent.travel_x = 0;
        m_flameEvent.travel_y = 0;
        m_flameEvent.lit_n = 0;
        m_flameEvent.total_n = m_system.n();
        m_flameEvent.percentageLit = 0;
        m_flameEvent.globalMix = m_system.mix();
        m_lit = true;
        m_litLastFrame = true;

        constexpr double fastFlameSpeed = units::distance(15, units::m);
        constexpr double slowFlameSpeed = units::distance(10, units::m);

        const double fuel_air_low = 0;
        const double fuel_air_high = 4.0 / 25;
        const double r = (double)rand() / RAND_MAX;
        const double s = ((equivalenceRatio - fuel_air_low) / (fuel_air_high - fuel_air_low)) * (r * 0.5 + 0.5);

        m_flameEvent.efficiency = 0.75 * (0.7 + 0.3 * ((double)rand() / RAND_MAX));
        //m_flameEvent.flameSpeed = 0.8 * (s * fastFlameSpeed + (1 - s) * slowFlameSpeed);

        const double turbulence = m_meanPistonSpeedToTurbulence->sampleTriangle(
            calculateMeanPistonSpeed());

        m_flameEvent.flameSpeed = m_fuel->flameSpeed(
            turbulence,
            afr,
            m_system.temperature(),
            m_system.pressure(),
            calculateFiringPressure(),
            units::pressure(160, units::psi));
        //m_flameEvent.efficiency = 1.0;
        //m_flameEvent.flameSpeed = fastFlameSpeed;

        if (rand() % 16 == 0) {
            //m_flameEvent.efficiency = 1;
            //m_flameEvent.flameSpeed = fastFlameSpeed;
        }
    }
}

void CombustionChamber::start() {
    m_system.start();
}

void CombustionChamber::update(double dt) {
    m_system.start();
    m_system.setVolume(getVolume());
    m_system.end();

    updateCycleStates();

    m_intakeFlowRate = m_head->intakeFlowRate(m_piston->getCylinderIndex());
    m_exhaustFlowRate = m_head->exhaustFlowRate(m_piston->getCylinderIndex());
}

void CombustionChamber::flow(double dt) {
    if (m_system.temperature() > m_peakTemperature) {
        m_peakTemperature = m_system.temperature();
    }

    m_system.flow(m_piston->getBlowbyK(), dt, m_crankcasePressure, units::celcius(25.0));

    const double start_n = m_system.n();

    const double intakeFlow = m_system.flow(
            m_intakeFlowRate,
            dt,
            &m_head->getIntake(m_piston->getCylinderIndex())->m_system);

    /*
    const double exhaustFlow = m_system.flow(
        m_exhaustFlowRate,
        dt,
        &m_head->getExhaustSystem(m_piston->getCylinderIndex())->m_system,
        -DBL_MAX,
        DBL_MAX);*/

    // temp
    m_exhaustRunner.start();

    const double molecularMass = units::mass(32, units::g);
    const double runnerStaticPressure = m_exhaustRunner.pressure();
    const double runnerVelocity =
        m_exhaustMomentum / (m_exhaustRunner.n() * molecularMass);
    const double machNumber = runnerVelocity / units::distance(345, units::m);
    const double hcr = m_exhaustRunner.heatCapacityRatio();
    //const double runnerTotalPressure =
    //    runnerStaticPressure
    //    * std::pow(1 + machNumber * machNumber * (hcr - 1) / 2, hcr / (hcr - 1));
    const double dynamicPressure =
        machNumber * machNumber * 0.5 * m_exhaustRunner.heatCapacityRatio() * runnerStaticPressure;

    GasSystem *source = (m_system.pressure() > runnerTotalPressure)
        ? &m_system
        : &m_exhaustRunner;

    const double exhaustFlow = m_system.flow(
            m_exhaustFlowRate,
            dt,
            &m_exhaustRunner);

    const double crossSectionArea = units::area(10.0, units::cm2);
    const double volumeOut =
        exhaustFlow * constants::R * m_system.temperature() / m_system.pressure();
    const double v = (volumeOut / crossSectionArea) / dt;
    const double mass = exhaustFlow * units::mass(32, units::g);
    const double newMomentum = v * mass;

    const double runnerToManifold = m_exhaustRunner.flow(
        m_exhaustFlowRate * 10,
        dt,
        &m_head->getExhaustSystem(m_piston->getCylinderIndex())->m_system);

    m_exhaustRunner.end();

    if (runnerToManifold > 0) {
        m_exhaustMomentum -= (m_exhaustMomentum / m_exhaustRunner.n()) * runnerToManifold;
    }

    if (exhaustFlow < 0) {
        m_exhaustMomentum -=
    }

    m_exhaustMomentum += newMomentum;

    // end temp

    if (std::abs(intakeFlow) > 1E-9 && m_lit) {
        m_lit = false;
    }

    m_exhaustFlow = exhaustFlow;
    m_lastTimestepTotalExhaustFlow += exhaustFlow;

    if (m_lit) {
        CylinderBank *bank = m_head->getCylinderBank();
        const double volume = getVolume();
        const double totalTravel_x = bank->getBore() / 2;
        const double totalTravel_y = volume / bank->boreSurfaceArea();
        const double expansion = volume / m_flameEvent.lastVolume;
        const double lastTravel_x = m_flameEvent.travel_x;
        const double lastTravel_y = m_flameEvent.travel_y * expansion;
        const double flameSpeed = m_flameEvent.flameSpeed;

        m_flameEvent.travel_x =
            std::fmin(lastTravel_x + dt * flameSpeed, totalTravel_x);
        m_flameEvent.travel_y =
            std::fmin(lastTravel_y + dt * flameSpeed, totalTravel_y);

        if (lastTravel_x < m_flameEvent.travel_x || lastTravel_y < m_flameEvent.travel_y) {
            const double burnedVolume =
                m_flameEvent.travel_x * m_flameEvent.travel_x
                * constants::pi * m_flameEvent.travel_y;
            const double prevBurnedVolume =
                lastTravel_x * lastTravel_x * constants::pi * lastTravel_y;
            const double litVolume = burnedVolume - prevBurnedVolume;
            const double n = (litVolume / volume) * m_system.n();

            const double fuelBurned =
                m_system.react(n * m_flameEvent.efficiency, m_flameEvent.globalMix);
            const double massFuelBurned = fuelBurned * m_fuel->getMolecularMass();
            m_system.changeEnergy(
                massFuelBurned * m_fuel->getEnergyDensity());

            m_flameEvent.lit_n += n;
            m_flameEvent.percentageLit += litVolume / volume;

            m_nBurntFuel += massFuelBurned;
        }
        else {
            m_lit = false;
        }

        m_flameEvent.lastVolume = volume;
    }
}

void CombustionChamber::end() {
    m_system.end();
}

double CombustionChamber::lastEventAfr() const {
    const double totalFuel = m_flameEvent.globalMix.p_fuel * m_flameEvent.total_n;
    const double totalOxygen = m_flameEvent.globalMix.p_o2 * m_flameEvent.total_n;
    const double totalInert = m_flameEvent.globalMix.p_inert * m_flameEvent.total_n;

    constexpr double octaneMolarMass = units::mass(114.23, units::g);
    constexpr double oxygenMolarMass = units::mass(31.9988, units::g);
    constexpr double nitrogenMolarMass = units::mass(28.014, units::g);

    if (totalFuel == 0) return 0;
    else {
        return
            (oxygenMolarMass * totalOxygen + totalInert * nitrogenMolarMass)
            / (totalFuel * octaneMolarMass);
    }
}

double CombustionChamber::calculateFrictionForce(double v_s) const {
    const double cylinderWallForce = m_piston->calculateCylinderWallForce();

    const double F_coul = m_frictionModel.frictionCoeff * cylinderWallForce;
    const double v_st = m_frictionModel.breakawayFrictionVelocity * constants::root_2;
    const double v_coul = m_frictionModel.breakawayFrictionVelocity / 10;
    const double F_brk = m_frictionModel.breakawayFriction;
    const double v = std::abs(v_s);

    const double F_0 = constants::root_2 * constants::e * (F_brk - F_coul);
    const double F_1 = v / v_st;
    const double F_2 = std::exp(-F_1 * F_1) * F_1;
    const double F_3 = F_coul * std::tanh(v / v_coul);
    const double F_4 = m_frictionModel.viscousFrictionCoefficient * v;

    return F_0 * F_2 + F_3 + F_4;
}

void CombustionChamber::updateCycleStates() {
    const double crankAngle = m_engine->getOutputCrankshaft()->getCycleAngle();
    const int i = std::round((crankAngle / (4 * constants::pi)) * (StateSamples - 1));

    m_pistonSpeed[i] = std::abs(pistonSpeed());
    m_pressure[i] = m_system.pressure();
}

void CombustionChamber::apply(atg_scs::SystemState *system) {
    CylinderBank *bank = m_head->getCylinderBank();
    const double area = (bank->getBore() * bank->getBore() / 4.0) * constants::pi;
    const double v_x = system->v_x[m_piston->m_body.index];
    const double v_y = system->v_y[m_piston->m_body.index];

    const double v_s =
        v_x * bank->getDx() + v_y * bank->getDy();

    const double pressureDifferential = m_system.pressure() - m_crankcasePressure;
    const double force = -area * pressureDifferential;

    if (std::isnan(force) || std::isinf(force)) {
        assert(false);
    }

    constexpr double limit = 1E-3;
    const double abs_v_s = std::fmin(std::abs(v_s), limit);
    const double attenuation = abs_v_s / limit;

    const double F = calculateFrictionForce(v_s) * attenuation;
    const double F_fric = (v_s > 0)
        ? -F
        : F;

    system->applyForce(
        0.0,
        0.0,
        (force + F_fric) * bank->getDx(),
        (force + F_fric) * bank->getDy(),
        m_piston->m_body.index);
}

double CombustionChamber::getFrictionForce() const {
    CylinderBank *bank = m_head->getCylinderBank();
    const double v_x = m_piston->m_body.v_x;
    const double v_y = m_piston->m_body.v_y;

    const double v_s =
        v_x * bank->getDx() + v_y * bank->getDy();

    return calculateFrictionForce(v_s);
}
